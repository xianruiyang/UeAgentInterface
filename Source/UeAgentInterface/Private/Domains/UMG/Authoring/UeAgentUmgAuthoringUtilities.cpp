// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringUtilities.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Misc/ScopeExit.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "MovieScene.h"
#include "Components/Widget.h"
#include "FileHelpers.h"
#include "Misc/SecureHash.h"
#include "WidgetBlueprint.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"

namespace UeAgentUmgAuthoringUtilities
{
	FGuid MakeStableWidgetVariableGuid(const FString& VariableName)
	{
		FGuid Result;
		const FString StableKey =
			TEXT("uai.umg.variable:") + VariableName.TrimStartAndEnd().ToLower();
		FGuid::ParseExact(
			FMD5::HashAnsiString(*StableKey),
			EGuidFormats::Digits,
			Result);
		return Result;
	}

	int32 SyncWidgetVariableGuidMap(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint)
		{
			return 0;
		}

		int32 FixupCount = 0;
		TSet<FName> SourceVariableNames;
		auto EnsureGuidForName =
			[WidgetBlueprint, &FixupCount, &SourceVariableNames](const FName VariableName)
		{
			if (VariableName.IsNone())
			{
				return;
			}
			SourceVariableNames.Add(VariableName);
			FGuid* ExistingGuid =
				WidgetBlueprint->WidgetVariableNameToGuidMap.Find(VariableName);
			if (!ExistingGuid || !ExistingGuid->IsValid())
			{
				WidgetBlueprint->WidgetVariableNameToGuidMap.Add(
					VariableName,
					MakeStableWidgetVariableGuid(VariableName.ToString()));
				++FixupCount;
			}
		};

		WidgetBlueprint->ForEachSourceWidget([&EnsureGuidForName](UWidget* Widget)
		{
			if (Widget)
			{
				EnsureGuidForName(Widget->GetFName());
			}
		});

		for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (Animation)
			{
				EnsureGuidForName(Animation->GetFName());
			}
		}

		TSet<FGuid> SeenGuids;
		for (auto It =
			WidgetBlueprint->WidgetVariableNameToGuidMap.CreateIterator(); It; ++It)
		{
			if (!SourceVariableNames.Contains(It.Key()))
			{
				It.RemoveCurrent();
				++FixupCount;
				continue;
			}

			if (!It.Value().IsValid() || SeenGuids.Contains(It.Value()))
			{
				It.Value() = MakeStableWidgetVariableGuid(It.Key().ToString());
				++FixupCount;
			}
			SeenGuids.Add(It.Value());
		}

		if (FixupCount > 0)
		{
			WidgetBlueprint->Modify();
			WidgetBlueprint->MarkPackageDirty();
		}
		return FixupCount;
	}

	bool SaveWidgetBlueprintPackage(
		UWidgetBlueprint* WidgetBlueprint,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!WidgetBlueprint)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("widget_blueprint_not_found"), TEXT("No widget Blueprint is available to save."), TEXT("animation_apply"));
			return false;
		}
		if (!UeAgentBlueprintAssetLifecycle::ValidateBlueprintPersistence(*WidgetBlueprint, OutError)) return false;
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(WidgetBlueprint->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError, TEXT("save"), TEXT("The widget Blueprint package could not be saved."));
			return false;
		}
		return true;
	}

	UWidget* FindWidgetByAnyName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || WidgetName.IsEmpty())
		{
			return nullptr;
		}

		const FString NameTrim = WidgetName.TrimStartAndEnd();
		if (UWidget* Exact =
			WidgetBlueprint->WidgetTree->FindWidget(FName(*NameTrim)))
		{
			return Exact;
		}

		TArray<UWidget*> Widgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (Widget
				&& Widget->GetName().Equals(NameTrim, ESearchCase::IgnoreCase))
			{
				return Widget;
			}
		}
		return nullptr;
	}

	UWidgetAnimation* FindWidgetAnimationByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& AnimationName)
	{
		if (!WidgetBlueprint || AnimationName.IsEmpty()) return nullptr;
		const FString Needle = AnimationName.TrimStartAndEnd();
		for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
		{
			if (Animation && (Animation->GetName().Equals(Needle, ESearchCase::IgnoreCase)
#if WITH_EDITOR
				|| Animation->GetDisplayLabel().Equals(Needle, ESearchCase::IgnoreCase)
#endif
			)) return Animation;
		}
		return nullptr;
	}

	bool SetMovieScenePlaybackRangeSeconds(UMovieScene* MovieScene, double StartSeconds, double DurationSeconds,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		auto Fail = [&](const TCHAR* Reason, const TCHAR* Field) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_animation_range_invalid"), Reason, TEXT("animation_apply"));
			OutError.Source.JsonPath = Field;
			return false;
		};
		if (!MovieScene) return Fail(TEXT("The animation has no movie scene in which to set the playback range."), TEXT("/"));
		if (!FMath::IsFinite(DurationSeconds) || DurationSeconds <= 0.0) {
			Fail(TEXT("The animation duration must be finite and greater than zero."), TEXT("/duration_seconds"));
			OutError.ActualValue = FMath::IsFinite(DurationSeconds) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(DurationSeconds)) : MakeShared<FJsonValueString>(LexToString(DurationSeconds));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("finite number > 0"));
			return false;
		}
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		if (!TickResolution.IsValid()) {
			Fail(TEXT("The animation movie-scene tick resolution is invalid."), TEXT("/"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TickResolution.ToPrettyText().ToString());
			return false;
		}
		int32 StartFrame = 0, EndFrame = 0;
		if (!UeAgentPropertyValueCodec::TryConvertInt32(StartSeconds * TickResolution.Numerator / TickResolution.Denominator,
			StartFrame, OutError, true)) {
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/start_seconds"));
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("umg_animation_range_invalid"), OutError,
				TEXT("animation_apply"), TEXT("The animation start time cannot be represented as a movie-scene frame."));
			OutError.ActualValue = FMath::IsFinite(StartSeconds) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(StartSeconds)) : MakeShared<FJsonValueString>(LexToString(StartSeconds));
			return false;
		}
		if (!UeAgentPropertyValueCodec::TryConvertInt32(FMath::CeilToDouble(
			(StartSeconds + DurationSeconds) * TickResolution.Numerator / TickResolution.Denominator), EndFrame, OutError)) {
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/duration_seconds"));
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("umg_animation_range_invalid"), OutError,
				TEXT("animation_apply"), TEXT("The animation start time plus duration cannot be represented as a movie-scene end frame."));
			const auto Values = MakeShared<FJsonObject>();
			Values->SetNumberField(TEXT("start_seconds"), StartSeconds);
			Values->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
			OutError.ActualValue = MakeShared<FJsonValueObject>(Values);
			return false;
		}
		if (EndFrame <= StartFrame) {
			if (StartFrame == MAX_int32) {
				Fail(TEXT("The animation range cannot include a frame after the maximum int32 start frame."), TEXT("/start_seconds"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(StartSeconds);
				return false;
			}
			EndFrame = StartFrame + 1;
		}
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
		return true;
	}

	bool FindOrCreateWidgetAnimationBinding(UWidgetBlueprint* WidgetBlueprint, UWidgetAnimation* TargetAnimation, UWidget* TargetWidget, UMovieScene*& OutMovieScene, FGuid& OutBindingGuid, bool& bOutBindingAlreadyExists, UE::UAI::Contract::FDiagnostic& OutError)
	{
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) {
			if (TargetWidget) OutError.CandidateName = TargetWidget->GetName();
			else if (TargetAnimation) OutError.CandidateName = TargetAnimation->GetName();
			OutError.SourceKind = TEXT("native_result");
			if (WidgetBlueprint) OutError.TargetPath = WidgetBlueprint->GetPathName();
			else if (TargetAnimation) OutError.TargetPath = TargetAnimation->GetPathName();
			else if (TargetWidget) OutError.TargetPath = TargetWidget->GetPathName();
		} };
		OutMovieScene = TargetAnimation ? TargetAnimation->GetMovieScene() : nullptr;
		OutBindingGuid.Invalidate(); bOutBindingAlreadyExists = false;
		if (!WidgetBlueprint || !TargetAnimation || !TargetWidget) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_widget_animation_context"), !WidgetBlueprint ? TEXT("The animation binding requires a widget Blueprint.") : !TargetAnimation ? TEXT("The animation binding requires an animation.") : TEXT("The animation binding requires an existing widget."), TEXT("animation_apply")); OutError.PropertyPath = !WidgetBlueprint ? TEXT("/widget_blueprint") : !TargetAnimation ? TEXT("/animation") : TEXT("/widget"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true); return false; }
		if (!OutMovieScene) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("movie_scene_not_found"), TEXT("The widget animation has no movie scene."), TEXT("animation_apply")); OutError.PropertyPath = TEXT("/movie_scene"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true); return false; }
		for (const FWidgetAnimationBinding& Binding : TargetAnimation->AnimationBindings)
		{
			if (!Binding.bIsRootWidget && Binding.SlotWidgetName.IsNone() && Binding.WidgetName == TargetWidget->GetFName())
			{ OutBindingGuid = Binding.AnimationGuid; bOutBindingAlreadyExists = true; break; }
		}
		if (!OutBindingGuid.IsValid() || !OutMovieScene->FindBinding(OutBindingGuid))
		{
			UUserWidget* Context = WidgetBlueprint->GeneratedClass
				? Cast<UUserWidget>(WidgetBlueprint->GeneratedClass->GetDefaultObject()) : nullptr;
			if (!Context) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("widget_animation_context_missing"), TEXT("The widget Blueprint has no generated UserWidget default object for animation binding."), TEXT("animation_apply")); OutError.PropertyPath = TEXT("/generated_class/user_widget_default"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true); return false; }
			OutBindingGuid = OutMovieScene->AddPossessable(TargetWidget->GetName(), TargetWidget->GetClass());
			if (!OutBindingGuid.IsValid()) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("add_widget_possessable_failed"), TEXT("The movie scene could not create a binding for the requested widget."), TEXT("animation_apply")); OutError.PropertyPath = TEXT("/widget_binding"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false); OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true); return false; }
			TargetAnimation->BindPossessableObject(OutBindingGuid, *TargetWidget, Context);
		}
		return true;
	}

	template<typename TrackType, typename SectionType>
	static bool FindOrCreateUmgSection(UMovieScene* MovieScene, const FGuid& BindingGuid, FName PropertyName, const FString& PropertyPath, TrackType*& OutTrack, SectionType*& OutSection, bool& bOutTrackAlreadyExists, const TCHAR* InvalidError, const TCHAR* TrackError, const TCHAR* SectionError, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutTrack = nullptr; OutSection = nullptr; bOutTrackAlreadyExists = false;
		auto Fail = [&](const TCHAR* Code, const TCHAR* Reason, UClass* ExpectedClass) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("animation_apply"));
			OutError.CandidateName = PropertyPath;
			OutError.SourceKind = TEXT("native_result");
			if (MovieScene) OutError.TargetPath = MovieScene->GetPathName();
			OutError.PropertyPath = ExpectedClass ? (OutTrack ? TEXT("/native_section") : TEXT("/native_track"))
				: !MovieScene ? TEXT("/movie_scene") : TEXT("/widget_binding");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			if (ExpectedClass) OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedClass->GetPathName());
			else OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		};
		if (!MovieScene || !BindingGuid.IsValid())
			return Fail(InvalidError, !MovieScene ? TEXT("The animation track requires a movie scene.") : TEXT("The animation track has no valid widget binding."), nullptr);
		OutTrack = MovieScene->FindTrack<TrackType>(BindingGuid, PropertyName);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack) {
			OutTrack = MovieScene->AddTrack<TrackType>(BindingGuid);
			if (!OutTrack) return Fail(TrackError, TEXT("The movie scene could not create the requested property track."), TrackType::StaticClass());
			OutTrack->SetPropertyNameAndPath(PropertyName, PropertyPath);
		}
		if (OutTrack->GetAllSections().Num() > 0) OutSection = Cast<SectionType>(OutTrack->GetAllSections()[0]);
		if (!OutSection) {
			OutSection = Cast<SectionType>(OutTrack->CreateNewSection());
			if (!OutSection) return Fail(SectionError, TEXT("The animation track could not create a section of the required type."), SectionType::StaticClass());
			OutTrack->AddSection(*OutSection);
		}
		return true;
	}

	bool FindOrCreateUmgTransformSection(UMovieScene* MovieScene, const FGuid& BindingGuid, UMovieScene2DTransformTrack*& OutTrack, UMovieScene2DTransformSection*& OutSection, bool& bOutTrackAlreadyExists, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutTrack = nullptr; OutSection = nullptr; bOutTrackAlreadyExists = false;
		const auto Fail = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Path, UClass* ExpectedClass = nullptr) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("animation_apply"));
			OutError.SourceKind = TEXT("native_result");
			if (MovieScene) OutError.TargetPath = MovieScene->GetPathName();
			OutError.CandidateName = TEXT("RenderTransform");
			OutError.PropertyPath = Path;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			if (ExpectedClass) OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedClass->GetPathName());
			else OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		};
		if (!MovieScene || !BindingGuid.IsValid()) return Fail(TEXT("invalid_widget_transform_context"),
			!MovieScene ? TEXT("The transform track requires a movie scene.") : TEXT("The transform track requires a valid widget binding."),
			!MovieScene ? TEXT("/movie_scene") : TEXT("/widget_binding"));
		OutTrack = MovieScene->FindTrack<UMovieScene2DTransformTrack>(BindingGuid);
		bOutTrackAlreadyExists = OutTrack != nullptr;
		if (!OutTrack) {
			OutTrack = MovieScene->AddTrack<UMovieScene2DTransformTrack>(BindingGuid);
			if (!OutTrack) return Fail(TEXT("create_umg_transform_track_failed"), TEXT("The movie scene could not create a RenderTransform track."),
				TEXT("/native_track"), UMovieScene2DTransformTrack::StaticClass());
			OutTrack->SetPropertyNameAndPath(TEXT("RenderTransform"), TEXT("RenderTransform"));
		}
		if (OutTrack->GetAllSections().Num() > 0) OutSection = Cast<UMovieScene2DTransformSection>(OutTrack->GetAllSections()[0]);
		if (!OutSection) {
			OutSection = Cast<UMovieScene2DTransformSection>(OutTrack->CreateNewSection());
			if (!OutSection) return Fail(TEXT("create_umg_transform_section_failed"), TEXT("The RenderTransform track could not create a 2D transform section."),
				TEXT("/native_section"), UMovieScene2DTransformSection::StaticClass());
			OutTrack->AddSection(*OutSection);
		}
		return true;
	}
	bool FindOrCreateUmgOpacitySection(UMovieScene* MovieScene, const FGuid& BindingGuid, UMovieSceneFloatTrack*& OutTrack, UMovieSceneFloatSection*& OutSection, bool& bOutTrackAlreadyExists, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return FindOrCreateUmgSection(MovieScene, BindingGuid, TEXT("RenderOpacity"), TEXT("RenderOpacity"), OutTrack, OutSection, bOutTrackAlreadyExists, TEXT("invalid_widget_opacity_context"), TEXT("create_umg_opacity_track_failed"), TEXT("create_umg_opacity_section_failed"), OutError);
	}
	bool FindOrCreateUmgFloatSection(UMovieScene* MovieScene, const FGuid& BindingGuid, FName PropertyName, const FString& PropertyPath, UMovieSceneFloatTrack*& OutTrack, UMovieSceneFloatSection*& OutSection, bool& bOutTrackAlreadyExists, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return FindOrCreateUmgSection(MovieScene, BindingGuid, PropertyName, PropertyPath, OutTrack, OutSection, bOutTrackAlreadyExists, TEXT("invalid_widget_float_context"), TEXT("create_umg_float_track_failed"), TEXT("create_umg_float_section_failed"), OutError);
	}
	bool FindOrCreateUmgColorSection(UMovieScene* MovieScene, const FGuid& BindingGuid, FName PropertyName, const FString& PropertyPath, UMovieSceneColorTrack*& OutTrack, UMovieSceneColorSection*& OutSection, bool& bOutTrackAlreadyExists, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return FindOrCreateUmgSection(MovieScene, BindingGuid, PropertyName, PropertyPath, OutTrack, OutSection, bOutTrackAlreadyExists, TEXT("invalid_widget_color_context"), TEXT("create_umg_color_track_failed"), TEXT("create_umg_color_section_failed"), OutError);
	}
}
