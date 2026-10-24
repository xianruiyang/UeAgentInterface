#include "Domains/UMG/Authoring/UeAgentUmgAnimationState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieScene.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"

namespace UeAgentUmgAuthoring
{
	using namespace UE::UAI::Authoring::Timeline;
	namespace
	{
		UE::UAI::Contract::FDiagnostic AnimationStateFailure(const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("authoring"), Reason);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			return Error;
		}

		bool ResolveWidgetBinding(UWidgetBlueprint* Blueprint, const FTimelineAnimationBinding& Binding,
			FMovieSceneObjectBinding& Out, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!Blueprint || !Blueprint->GeneratedClass) {
				Error = AnimationStateFailure(TEXT("umg_animation_blueprint_context_missing"),
					!Blueprint ? TEXT("Animation binding requires a WidgetBlueprint context.")
					: TEXT("The WidgetBlueprint has no generated class for binding resolution."),
					!Blueprint ? TEXT("/widget_blueprint") : TEXT("/generated_class"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Blueprint ? Blueprint->GetPathName() : FString());
				return false;
			}
			Out.Context = Cast<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject());
			if (!Out.Context) {
				Error = AnimationStateFailure(TEXT("umg_animation_user_widget_context_missing"),
					TEXT("The generated class has no UserWidget class default object."),
					TEXT("/generated_class/default_object"), MakeShared<FJsonValueString>(Blueprint->GeneratedClass->GetPathName()),
					MakeShared<FJsonValueString>(TEXT("class default object derived from /Script/UMG.UserWidget")), Blueprint->GetPathName());
				return false;
			}
			UWidget* Widget = nullptr;
			if (Binding.BindingKind == TEXT("root_widget") && Binding.ObjectRef.IsEmpty()) Out.Object = Out.Context;
			else {
				Widget = UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(Blueprint, Binding.ObjectRef);
				if (Widget && Binding.BindingKind == TEXT("widget")) Out.Object = Widget;
				else if (Widget && Binding.BindingKind == TEXT("slot")) Out.Object = Widget->Slot;
			}
			if (!Out.Object) {
				const bool bKindSupported = Binding.BindingKind == TEXT("widget") || Binding.BindingKind == TEXT("slot") || Binding.BindingKind == TEXT("root_widget");
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("binding_kind"), Binding.BindingKind);
				Actual->SetStringField(TEXT("object_ref"), Binding.ObjectRef);
				Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("umg_animation_binding_object_unresolved"),
					!bKindSupported ? TEXT("/binding_kind") : TEXT("/object_ref"),
					MakeShared<FJsonValueString>(TEXT("widget, its existing slot, or root_widget with an empty object_ref")),
					MakeShared<FJsonValueObject>(Actual), TEXT("authoring"),
					!bKindSupported ? TEXT("This WidgetBlueprint animation does not support the requested binding kind.")
					: Binding.BindingKind == TEXT("root_widget") ? TEXT("A root_widget binding requires an empty object_ref.")
					: !Widget ? TEXT("No widget matches the animation binding object_ref.")
					: TEXT("The matched widget has no panel slot to bind."));
				Error.TargetPath = Blueprint->GetPathName();
				if (Binding.SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Binding.SourceRecord);
				if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty() && Error.SourceKind != TEXT("command_input"))
					Error.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}
	}

	bool TryQueryAnimationTrackCandidates(const FString& Target, const FString& AnimationName,
		const FString& Query, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& BindingKind, const FString& ObjectRef, const FString& PropertyPath)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(UeAgentBlueprintAssetLifecycle::LoadBlueprint(Target));
		UWidgetAnimation* Animation = UeAgentUmgAuthoringUtilities::FindWidgetAnimationByAnyName(Blueprint, AnimationName);
		if (!Animation || !Animation->GetMovieScene()) {
			OutError = AnimationStateFailure(TEXT("umg_animation_target_unresolved"),
				!Blueprint ? TEXT("The query target could not be loaded as a WidgetBlueprint.")
				: !Animation ? TEXT("The WidgetBlueprint has no animation matching animation_name.")
				: TEXT("The matched widget animation has no MovieScene."),
				!Blueprint ? TEXT("/target") : !Animation ? TEXT("/animation_name") : TEXT("/movie_scene"),
				MakeShared<FJsonValueString>(!Blueprint ? Target : AnimationName),
				MakeShared<FJsonValueString>(!Blueprint ? TEXT("WidgetBlueprint asset") : !Animation ? TEXT("existing widget animation") : TEXT("widget animation with a MovieScene")), Target);
			return false;
		}
		FMovieSceneObjectBinding Object;
		if (!PropertyPath.IsEmpty()) {
			FTimelineAnimationBinding Binding; Binding.BindingKind = BindingKind; Binding.ObjectRef = ObjectRef;
			if (!ResolveWidgetBinding(Blueprint, Binding, Object, OutError)) return false;
		}
		return TryQueryMovieSceneTrackCandidates(*Animation->GetMovieScene(), Query, OutPayload, OutError, Object.Object, PropertyPath);
	}

	bool TryReadAnimationState(const UWidgetAnimation& Animation, const FString& CanonicalAssetPath,
		TSharedPtr<FJsonObject>& OutState, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutState.Reset();
		const UMovieScene* Scene = Animation.GetMovieScene();
		if (!Scene) {
			OutError = AnimationStateFailure(TEXT("umg_animation_movie_scene_missing"),
				TEXT("The widget animation has no MovieScene to capture."), TEXT("/movie_scene"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Animation.GetPathName());
			return false;
		}
		TMap<FGuid, FMovieSceneBindingIdentity> Identities;
		for (int32 Index = 0; Index < Animation.AnimationBindings.Num(); ++Index) {
			const auto& Binding = Animation.AnimationBindings[Index];
			FMovieSceneBindingIdentity Identity;
			Identity.Kind = Binding.bIsRootWidget ? TEXT("root_widget")
				: Binding.SlotWidgetName.IsNone() ? TEXT("widget") : TEXT("slot");
			Identity.ObjectRef = Binding.bIsRootWidget ? FString() : Binding.WidgetName.ToString();
			if (const auto* Previous = Identities.Find(Binding.AnimationGuid)) {
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("binding_kind"), Identity.Kind);
				Actual->SetStringField(TEXT("object_ref"), Identity.ObjectRef);
				Actual->SetStringField(TEXT("previous_binding_kind"), Previous->Kind);
				Actual->SetStringField(TEXT("previous_object_ref"), Previous->ObjectRef);
				OutError = AnimationStateFailure(TEXT("umg_animation_binding_identity_duplicate"),
					TEXT("Two widget animation bindings refer to the same native animation binding."),
					TEXT("/animation_bindings/") + FString::FromInt(Index), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("one widget animation binding per native animation binding")), Animation.GetPathName());
				OutError.SubjectId = Binding.AnimationGuid.ToString(EGuidFormats::Digits);
				return false;
			}
			Identities.Add(Binding.AnimationGuid, MoveTemp(Identity));
		}
		FString AssetPath = CanonicalAssetPath;
		int32 Dot;
		if (AssetPath.FindChar(TEXT('.'), Dot)) AssetPath.LeftInline(Dot);
		TSharedPtr<FJsonObject> Structure;
		FUeAgentTimelineAnimationExecutionError Error;
		if (!TryReadMovieSceneStructure(*Scene, TEXT("widget_animation:") + AssetPath + TEXT(":") + Animation.GetName(),
			Identities, Structure, Error))
		{
			if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			OutError = MoveTemp(Error); return false;
		}
		OutState = Structure;
		OutState->SetStringField(TEXT("name"), Animation.GetName());
		OutState->SetStringField(TEXT("display_label"), Animation.GetDisplayLabel());
		return true;
	}

	bool TryUpdateAnimationState(UWidgetAnimation& Animation, const FString& CanonicalAssetPath,
		const TSharedPtr<FJsonObject>& DesiredScene, UE::UAI::Contract::FDiagnostic& OutError, FMovieSceneCreationState* CreationState)
	{
		TSharedPtr<FJsonObject> CurrentState;
		if (!TryReadAnimationState(Animation, CanonicalAssetPath, CurrentState, OutError)) return false;
		// New animation records have no captured scene. Keep defaults owned by the
		// native MovieScene, just as an omitted field retains an existing scene value.
		auto DesiredState = MakeShared<FJsonObject>(*CurrentState);
		for (const auto& Field : DesiredScene->Values) DesiredState->SetField(Field.Key, Field.Value);
		// Clock identity is private context, never an authored requirement. A newly
		// supplied playback range has no captured identity to restore during import.
		const TSharedPtr<FJsonObject>* SuppliedRange = nullptr;
		if (DesiredScene->TryGetObjectField(TEXT("playback_range"), SuppliedRange))
		{
			auto Range = MakeShared<FJsonObject>(**SuppliedRange);
			const FString ClockId = CurrentState->GetObjectField(TEXT("playback_range"))->GetStringField(TEXT("clock_id"));
			if (!Range->HasField(TEXT("clock_id"))) Range->SetStringField(TEXT("clock_id"), ClockId);
			for (const TCHAR* BoundName : {TEXT("lower"), TEXT("upper")})
			{
				const TSharedPtr<FJsonObject>* Bound = nullptr;
				const TSharedPtr<FJsonObject>* Point = nullptr;
				if (!Range->TryGetObjectField(BoundName, Bound) || !(*Bound)->TryGetObjectField(TEXT("point"), Point)) continue;
				auto RestoredPoint = MakeShared<FJsonObject>(**Point);
				if (!RestoredPoint->HasField(TEXT("clock_id"))) RestoredPoint->SetStringField(TEXT("clock_id"), ClockId);
				auto RestoredBound = MakeShared<FJsonObject>(**Bound);
				RestoredBound->SetObjectField(TEXT("point"), RestoredPoint);
				Range->SetObjectField(BoundName, RestoredBound);
			}
			DesiredState->SetObjectField(TEXT("playback_range"), Range);
		}
		FTimelineAnimationStructure Current, Desired;
		FUeAgentTimelineAnimationExecutionError Error;
		auto ResolveBinding = [&](const FTimelineAnimationBinding& Binding, FMovieSceneObjectBinding& Out, UE::UAI::Contract::FDiagnostic& ErrorText) {
			return ResolveWidgetBinding(Animation.GetTypedOuter<UWidgetBlueprint>(), Binding, Out, ErrorText);
		};
		if (!ParseStructure(CurrentState, Current, Error)) {
			if (Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			OutError = MoveTemp(Error); return false;
		}
		if (!ParseStructure(DesiredState, Desired, Error)
			|| !TryUpdateMovieSceneValues(*Animation.GetMovieScene(), Current, Desired, Error, &Animation, ResolveBinding, CreationState))
		{ OutError = MoveTemp(Error); return false; }
		return true;
	}
}
