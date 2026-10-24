// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UWidget;
class UWidgetBlueprint;
class UMovieScene;
class UMovieScene2DTransformSection;
class UMovieScene2DTransformTrack;
class UMovieSceneColorSection;
class UMovieSceneColorTrack;
class UMovieSceneFloatSection;
class UMovieSceneFloatTrack;
class UWidgetAnimation;

namespace UeAgentUmgAuthoringUtilities
{
	FGuid MakeStableWidgetVariableGuid(const FString& VariableName);
	int32 SyncWidgetVariableGuidMap(UWidgetBlueprint* WidgetBlueprint);
	bool SaveWidgetBlueprintPackage(
		UWidgetBlueprint* WidgetBlueprint,
		UE::UAI::Contract::FDiagnostic& OutError);
	UWidget* FindWidgetByAnyName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& WidgetName);
	UWidgetAnimation* FindWidgetAnimationByAnyName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& AnimationName);
	bool SetMovieScenePlaybackRangeSeconds(
		UMovieScene* MovieScene,
		double StartSeconds,
		double DurationSeconds,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool FindOrCreateWidgetAnimationBinding(
		UWidgetBlueprint* WidgetBlueprint,
		UWidgetAnimation* TargetAnimation,
		UWidget* TargetWidget,
		UMovieScene*& OutMovieScene,
		FGuid& OutBindingGuid,
		bool& bOutBindingAlreadyExists,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool FindOrCreateUmgTransformSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		UMovieScene2DTransformTrack*& OutTrack,
		UMovieScene2DTransformSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool FindOrCreateUmgOpacitySection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		UMovieSceneFloatTrack*& OutTrack,
		UMovieSceneFloatSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool FindOrCreateUmgFloatSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		FName PropertyName,
		const FString& PropertyPath,
		UMovieSceneFloatTrack*& OutTrack,
		UMovieSceneFloatSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		UE::UAI::Contract::FDiagnostic& OutError);
	bool FindOrCreateUmgColorSection(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		FName PropertyName,
		const FString& PropertyPath,
		UMovieSceneColorTrack*& OutTrack,
		UMovieSceneColorSection*& OutSection,
		bool& bOutTrackAlreadyExists,
		UE::UAI::Contract::FDiagnostic& OutError);
}
