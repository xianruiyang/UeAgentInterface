// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieScenePropertyTrack.h"

class ULevelSequence;
class UAnimSequenceBase;
class UMovieScene2DTransformSection;
class UMovieScene2DTransformTrack;
class UMovieSceneColorSection;
class UMovieSceneColorTrack;
class UMovieSceneFloatSection;
class UMovieSceneFloatTrack;
class UMovieSceneSection;
class UMovieSceneTrack;
class UWidget;
class UWidgetAnimation;
class UWidgetBlueprint;
class FLevelEditorViewportClient;
struct FMovieSceneDoubleChannel;
struct FMovieSceneFloatChannel;

namespace UeAgentSequenceOps
{
	enum class ESequencePropertyTrackType : uint8
	{
		Bool,
		Byte,
		Double,
		Float,
		Integer,
		Color,
		Vector,
		Rotator,
		ActorReference,
		Object,
		String
	};

	enum class ESequenceVectorPrecision : uint8
	{
		Float,
		Double
	};

	struct FLevelSequenceFrameRequest
	{
		int32 FrameIndex;
		double TimeSeconds;
		FFrameNumber EvaluationFrame;
	};

	bool ParseSequencePropertyTrackType(const FString& InText, ESequencePropertyTrackType& OutType);
	bool ParseSequenceVectorPrecision(const FString& InText, ESequenceVectorPrecision& OutPrecision);
	void InferSequenceVectorDefaultsFromTypeText(const FString& InText, bool& bOutHasPrecision, ESequenceVectorPrecision& OutPrecision, bool& bOutHasChannels, int32& OutChannels);
	FString NormalizeAssetPath(const FString& InPath);
	FString ToObjectPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	bool AssetExists(const FString& InPath);
	ULevelSequence* LoadLevelSequence(const FString& InPath);
	UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath);
	UAnimSequenceBase* LoadAnimSequenceBase(const FString& InPath);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError);
	bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError);
	bool LoadOptionalJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError);
	bool SetMovieScenePlaybackRangeSeconds(UMovieScene* MovieScene, double StartSeconds, double DurationSeconds);
	bool TryGetSequenceNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue);
	bool AddLevelSequenceFrameRequest(UMovieScene* MovieScene, double RelativeTimeSeconds, int32 FrameIndex, TArray<FLevelSequenceFrameRequest>& OutFrames, FString& OutError);
	bool ResolveLevelSequenceFrameSequence(ULevelSequence* Sequence, const TSharedPtr<FJsonObject>& Params, TArray<FLevelSequenceFrameRequest>& OutFrames, FString& OutError);
	FLevelEditorViewportClient* GetPreferredLevelViewportClient();
	UWidgetAnimation* FindWidgetAnimationByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& InName);
	UWidget* FindWidgetByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& InName);
	int32 GetFloatChannelKeyCount(const FMovieSceneFloatChannel& Channel);
	int32 GetDoubleChannelKeyCount(const FMovieSceneDoubleChannel& Channel);
	FString LexToString(ESequenceVectorPrecision Precision);
	void PopulateSectionRangeFields(const UMovieSceneSection* Section, TSharedPtr<FJsonObject>& OutObject);
	TSharedPtr<FJsonObject> BuildMovieSceneTrackSummary(UMovieScene* MovieScene, UMovieSceneTrack* Track);
	TSharedPtr<FJsonObject> BuildMovieSceneBindingSummary(UMovieScene* MovieScene, const FMovieSceneBinding& Binding);
	bool FindOrCreateWidgetAnimationBinding(UWidgetBlueprint* WidgetBlueprint, UWidgetAnimation* TargetAnimation, UWidget* TargetWidget, UMovieScene*& OutMovieScene, FGuid& OutBindingGuid, bool& bOutBindingAlreadyExists, FString& OutError);
	bool FindOrCreateUmgTransformSection(UMovieScene* MovieScene, const FGuid& BindingGuid, UMovieScene2DTransformTrack*& OutTrack, UMovieScene2DTransformSection*& OutSection, bool& bOutTrackAlreadyExists, FString& OutError);
	bool FindOrCreateUmgOpacitySection(UMovieScene* MovieScene, const FGuid& BindingGuid, UMovieSceneFloatTrack*& OutTrack, UMovieSceneFloatSection*& OutSection, bool& bOutTrackAlreadyExists, FString& OutError);
	bool FindOrCreateUmgFloatSection(UMovieScene* MovieScene, const FGuid& BindingGuid, const FName PropertyName, const FString& PropertyPath, UMovieSceneFloatTrack*& OutTrack, UMovieSceneFloatSection*& OutSection, bool& bOutTrackAlreadyExists, FString& OutError);
	bool FindOrCreateUmgColorSection(UMovieScene* MovieScene, const FGuid& BindingGuid, const FName PropertyName, const FString& PropertyPath, UMovieSceneColorTrack*& OutTrack, UMovieSceneColorSection*& OutSection, bool& bOutTrackAlreadyExists, FString& OutError);

	template<typename TTrackType>
	TTrackType* FindPropertyTrackByNameAndPath(UMovieScene* MovieScene, const FGuid& BindingGuid, const FName PropertyName, const FString& PropertyPath)
	{
		if (!MovieScene)
		{
			return nullptr;
		}

		if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				TTrackType* TypedTrack = Cast<TTrackType>(Track);
				UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
				if (!TypedTrack || !PropertyTrack)
				{
					continue;
				}

				if (PropertyTrack->GetPropertyName() == PropertyName &&
					(PropertyPath.IsEmpty() || PropertyTrack->GetPropertyPath().ToString().Equals(PropertyPath, ESearchCase::IgnoreCase)))
				{
					return TypedTrack;
				}
			}
		}

		return MovieScene->FindTrack<TTrackType>(BindingGuid, PropertyName);
	}
}
