// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"

namespace UeAgentUmgAuthoring
{
	namespace
	{
		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError))
			{
				return false;
			}
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.RootOwnerPath = Selectors.FindRef(TEXT("asset_path"));
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		bool AppendPropertyRequests(
			const UObject* Object,
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& BaseSelectors,
			const TSet<FName>& Excluded,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			if (!Object) return true;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = SemanticId;
			Options.ExcludedProperties = Excluded;
			Options.BuildSnapshotIdentity =
				[ModelId, TargetKind, BaseSelectors](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					FString& IdentityError)
			{
				TMap<FString, FString> Selectors = BaseSelectors;
				Selectors.Add(TEXT("property_path"), PropertyPath);
				return BuildIdentity(ModelId, TargetKind, Selectors, OutIdentity, IdentityError);
			};
			return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, Options, OutRequests, OutError);
		}

		void CollectWidgets(UWidget* Widget, TArray<UWidget*>& OutWidgets)
		{
			if (!Widget) return;
			OutWidgets.Add(Widget);
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					CollectWidgets(Panel->GetChildAt(Index), OutWidgets);
				}
			}
		}

		void BuildParentMaps(
			UWidget* Widget,
			const FString& ParentName,
			TMap<const UWidget*, FString>& OutParents,
			TMap<const UWidget*, int32>& OutIndices)
		{
			if (!Widget) return;
			OutParents.Add(Widget, ParentName);
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					if (UWidget* Child = Panel->GetChildAt(Index))
					{
						OutIndices.Add(Child, Index);
						BuildParentMaps(Child, Widget->GetName(), OutParents, OutIndices);
					}
				}
			}
		}

		TMap<FString, FString> WidgetSelectors(
			const UWidgetBlueprint& Blueprint,
			const UWidget& Widget,
			const FString& CanonicalAssetPath)
		{
			const FGuid* Guid = Blueprint.WidgetVariableNameToGuidMap.Find(Widget.GetFName());
			return {
				{ TEXT("asset_path"), CanonicalAssetPath.IsEmpty()
					? Blueprint.GetOutermost()->GetName() : CanonicalAssetPath },
				{ TEXT("widget_guid"), Guid ? GuidText(*Guid) : FString() },
				{ TEXT("widget_class"), Widget.GetClass()->GetPathName() },
				{ TEXT("widget_name"), Widget.GetName() },
			};
		}

		TSharedRef<FJsonObject> BuildWidgetState(
			const UWidget& Widget,
			const FString& ParentName,
			const int32 Index)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("name"), Widget.GetName());
			State->SetStringField(TEXT("class"), Widget.GetClass()->GetPathName());
			State->SetBoolField(TEXT("make_variable"), Widget.bIsVariable);
			if (!ParentName.IsEmpty())
			{
				State->SetStringField(TEXT("parent"), ParentName);
				State->SetNumberField(TEXT("index"), Index);
			}
			return State;
		}

		TSharedRef<FJsonObject> BuildSlotState(const UPanelSlot& Slot)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("class"), Slot.GetClass()->GetPathName());
			return State;
		}

		TSharedRef<FJsonObject> BuildBindingState(const FDelegateEditorBinding& Binding)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("widget_name"), Binding.ObjectName);
			State->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
			if (Binding.Kind == EBindingKind::Property)
			{
				State->SetStringField(TEXT("binding_kind"), TEXT("property_variable"));
				State->SetStringField(TEXT("source_variable_name"), Binding.SourceProperty.ToString());
			}
			else
			{
				State->SetStringField(TEXT("binding_kind"), TEXT("function"));
				State->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
			}
			return State;
		}

		void BuildFloatKeyMap(const FMovieSceneFloatChannel& Channel, TMap<int32, float>& OutValues)
		{
			const TArrayView<const FFrameNumber> Times = Channel.GetData().GetTimes();
			const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetData().GetValues();
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				OutValues.Add(Times[Index].Value, Values[Index].Value);
			}
		}

		TSharedPtr<FJsonObject> BuildAnimationTrackState(
			const FFrameRate& TickResolution,
			const FString& WidgetName,
			UMovieSceneTrack* Track)
		{
			if (UMovieScene2DTransformTrack* TransformTrack = Cast<UMovieScene2DTransformTrack>(Track))
			{
				const TArray<UMovieSceneSection*>& Sections = TransformTrack->GetAllSections();
				UMovieScene2DTransformSection* Section = Sections.Num() > 0
					? Cast<UMovieScene2DTransformSection>(Sections[0]) : nullptr;
				if (!Section) return nullptr;
				TMap<int32, float> Channels[7];
				BuildFloatKeyMap(Section->Translation[0], Channels[0]);
				BuildFloatKeyMap(Section->Translation[1], Channels[1]);
				BuildFloatKeyMap(Section->Scale[0], Channels[2]);
				BuildFloatKeyMap(Section->Scale[1], Channels[3]);
				BuildFloatKeyMap(Section->Shear[0], Channels[4]);
				BuildFloatKeyMap(Section->Shear[1], Channels[5]);
				BuildFloatKeyMap(Section->Rotation, Channels[6]);
				TSet<int32> Frames;
				for (const TMap<int32, float>& Channel : Channels)
				{
					for (const TPair<int32, float>& Pair : Channel) Frames.Add(Pair.Key);
				}
				TArray<int32> SortedFrames = Frames.Array();
				SortedFrames.Sort();
				static const TCHAR* Fields[] = {
					TEXT("translation_x"), TEXT("translation_y"), TEXT("scale_x"),
					TEXT("scale_y"), TEXT("shear_x"), TEXT("shear_y"), TEXT("angle")
				};
				TArray<TSharedPtr<FJsonValue>> Keys;
				for (const int32 Frame : SortedFrames)
				{
					TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
					Key->SetNumberField(TEXT("frame"), Frame);
					Key->SetNumberField(
						TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(Frame))));
					for (int32 ChannelIndex = 0; ChannelIndex < UE_ARRAY_COUNT(Fields); ++ChannelIndex)
					{
						if (const float* Value = Channels[ChannelIndex].Find(Frame))
						{
							Key->SetNumberField(Fields[ChannelIndex], *Value);
						}
					}
					Keys.Add(MakeShared<FJsonValueObject>(Key));
				}
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("widget_name"), WidgetName);
				State->SetStringField(TEXT("track_kind"), TEXT("transform"));
				State->SetArrayField(TEXT("keys"), Keys);
				return State;
			}

			if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
			{
				const TArray<UMovieSceneSection*>& Sections = FloatTrack->GetAllSections();
				UMovieSceneFloatSection* Section = Sections.Num() > 0
					? Cast<UMovieSceneFloatSection>(Sections[0]) : nullptr;
				if (!Section) return nullptr;
				TMap<int32, float> Values;
				BuildFloatKeyMap(Section->GetChannel(), Values);
				TArray<int32> Frames;
				Values.GetKeys(Frames);
				Frames.Sort();
				TArray<TSharedPtr<FJsonValue>> Keys;
				for (const int32 Frame : Frames)
				{
					TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
					Key->SetNumberField(TEXT("frame"), Frame);
					Key->SetNumberField(
						TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(Frame))));
					Key->SetNumberField(TEXT("value"), Values.FindChecked(Frame));
					Keys.Add(MakeShared<FJsonValueObject>(Key));
				}
				const FString PropertyPath = FloatTrack->GetPropertyPath().ToString();
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("widget_name"), WidgetName);
				State->SetStringField(
					TEXT("track_kind"),
					PropertyPath.Equals(TEXT("RenderOpacity"), ESearchCase::IgnoreCase)
						? TEXT("opacity") : TEXT("float_property"));
				State->SetStringField(TEXT("property_name"), FloatTrack->GetPropertyName().ToString());
				State->SetStringField(TEXT("property_path"), PropertyPath);
				State->SetArrayField(TEXT("keys"), Keys);
				return State;
			}

			if (UMovieSceneColorTrack* ColorTrack = Cast<UMovieSceneColorTrack>(Track))
			{
				const TArray<UMovieSceneSection*>& Sections = ColorTrack->GetAllSections();
				UMovieSceneColorSection* Section = Sections.Num() > 0
					? Cast<UMovieSceneColorSection>(Sections[0]) : nullptr;
				if (!Section) return nullptr;
				TMap<int32, float> Channels[4];
				BuildFloatKeyMap(Section->GetRedChannel(), Channels[0]);
				BuildFloatKeyMap(Section->GetGreenChannel(), Channels[1]);
				BuildFloatKeyMap(Section->GetBlueChannel(), Channels[2]);
				BuildFloatKeyMap(Section->GetAlphaChannel(), Channels[3]);
				TSet<int32> Frames;
				for (const TMap<int32, float>& Channel : Channels)
				{
					for (const TPair<int32, float>& Pair : Channel) Frames.Add(Pair.Key);
				}
				TArray<int32> SortedFrames = Frames.Array();
				SortedFrames.Sort();
				static const TCHAR* Fields[] = {
					TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha")
				};
				TArray<TSharedPtr<FJsonValue>> Keys;
				for (const int32 Frame : SortedFrames)
				{
					TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
					Key->SetNumberField(TEXT("frame"), Frame);
					Key->SetNumberField(
						TEXT("time_seconds"), TickResolution.AsSeconds(FFrameTime(FFrameNumber(Frame))));
					for (int32 ChannelIndex = 0; ChannelIndex < UE_ARRAY_COUNT(Fields); ++ChannelIndex)
					{
						if (const float* Value = Channels[ChannelIndex].Find(Frame))
						{
							Key->SetNumberField(Fields[ChannelIndex], *Value);
						}
					}
					Keys.Add(MakeShared<FJsonValueObject>(Key));
				}
				const FString PropertyPath = ColorTrack->GetPropertyPath().ToString();
				FString Kind = TEXT("color_property");
				if (PropertyPath.Equals(TEXT("ColorAndOpacity"), ESearchCase::IgnoreCase)) Kind = TEXT("color");
				else if (PropertyPath.Equals(TEXT("BackgroundColor"), ESearchCase::IgnoreCase)) Kind = TEXT("background_color");
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("widget_name"), WidgetName);
				State->SetStringField(TEXT("track_kind"), Kind);
				State->SetStringField(TEXT("property_name"), ColorTrack->GetPropertyName().ToString());
				State->SetStringField(TEXT("property_path"), PropertyPath);
				State->SetArrayField(TEXT("keys"), Keys);
				return State;
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> BuildAnimationState(const UWidgetAnimation& Animation)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("name"), Animation.GetName());
#if WITH_EDITOR
			State->SetStringField(TEXT("display_label"), Animation.GetDisplayLabel());
#endif
			State->SetNumberField(TEXT("start_seconds"), Animation.GetStartTime());
			State->SetNumberField(TEXT("duration_seconds"), Animation.GetEndTime() - Animation.GetStartTime());
			TArray<TSharedPtr<FJsonValue>> Tracks;
			if (UMovieScene* MovieScene = Animation.GetMovieScene())
			{
				State->SetNumberField(TEXT("display_rate_num"), MovieScene->GetDisplayRate().Numerator);
				State->SetNumberField(TEXT("display_rate_den"), MovieScene->GetDisplayRate().Denominator);
				for (const FWidgetAnimationBinding& Binding : Animation.AnimationBindings)
				{
					if (const FMovieSceneBinding* SceneBinding = MovieScene->FindBinding(Binding.AnimationGuid))
					{
						for (UMovieSceneTrack* Track : SceneBinding->GetTracks())
						{
							if (TSharedPtr<FJsonObject> TrackState = BuildAnimationTrackState(
								MovieScene->GetTickResolution(), Binding.WidgetName.ToString(), Track))
							{
								Tracks.Add(MakeShared<FJsonValueObject>(TrackState));
							}
						}
					}
				}
			}
			State->SetArrayField(TEXT("tracks"), Tracks);
			return State;
		}

		TSharedRef<FJsonObject> BuildAssetVerificationState(
			const UWidgetBlueprint& Blueprint,
			const FString& CanonicalAssetPath)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("asset_path"),
				CanonicalAssetPath.IsEmpty() ? Blueprint.GetOutermost()->GetName() : CanonicalAssetPath);
			State->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
			State->SetStringField(
				TEXT("parent_class"), Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());

			TMap<const UWidget*, FString> Parents;
			TMap<const UWidget*, int32> Indices;
			if (Blueprint.WidgetTree) BuildParentMaps(Blueprint.WidgetTree->RootWidget, FString(), Parents, Indices);
			TArray<FString> WidgetKeys;
			TArray<UWidget*> Widgets;
			if (Blueprint.WidgetTree) CollectWidgets(Blueprint.WidgetTree->RootWidget, Widgets);
			for (const UWidget* Widget : Widgets)
			{
				if (!Widget) continue;
				WidgetKeys.Add(FString::Printf(
					TEXT("%s|%s|%s|%d|%s"),
					*Widget->GetName(),
					*Widget->GetClass()->GetPathName(),
					*Parents.FindRef(Widget),
					Indices.FindRef(Widget),
					Widget->Slot ? *Widget->Slot->GetClass()->GetPathName() : TEXT("")));
			}
			WidgetKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> WidgetValues;
			for (const FString& Key : WidgetKeys) WidgetValues.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("widgets"), WidgetValues);

			TArray<FString> BindingKeys;
			for (const FDelegateEditorBinding& Binding : Blueprint.Bindings)
			{
				BindingKeys.Add(UeAgentAuthoringState::MakeStableCanonicalValueKey(
					MakeShared<FJsonValueObject>(BuildBindingState(Binding))));
			}
			BindingKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> BindingValues;
			for (const FString& Key : BindingKeys) BindingValues.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("bindings"), BindingValues);

			TArray<FString> AnimationKeys;
			for (const UWidgetAnimation* Animation : Blueprint.Animations)
			{
				if (Animation)
				{
					AnimationKeys.Add(UeAgentAuthoringState::MakeStableCanonicalValueKey(
						MakeShared<FJsonValueObject>(BuildAnimationState(*Animation))));
				}
			}
			AnimationKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> AnimationValues;
			for (const FString& Key : AnimationKeys) AnimationValues.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("animations"), AnimationValues);
			return State;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Provenance = StateAccessorId;
			OutValue.bEditable = Request.SemanticId != ExportTargetSemanticId;
			OutError.Reset();
			if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true"))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ParentClassSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint || !Blueprint->ParentClass)
				{
					OutError = TEXT("umg_parent_class_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Type.Kind = TEXT("string");
				OutValue.Value = MakeShared<FJsonValueString>(Blueprint->ParentClass->GetPathName());
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			OutValue.Type.Kind = TEXT("object");
			if (Request.SemanticId == AssetSemanticId || Request.SemanticId == ExportTargetSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("target_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedRef<FJsonObject> State = Request.SemanticId == AssetSemanticId
					? BuildAssetVerificationState(
						*Blueprint, Request.TargetIdentity.GetSelector(TEXT("asset_path")))
					: MakeShared<FJsonObject>();
				if (Request.SemanticId == ExportTargetSemanticId)
				{
					State->SetStringField(
						TEXT("asset_path"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
					State->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == WidgetSemanticId)
			{
				const UWidget* Widget = Cast<UWidget>(Request.RootObject);
				if (!Widget)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("widget_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildWidgetState(
					*Widget,
					Request.Context.FindRef(TEXT("parent_name")),
					FCString::Atoi(*Request.Context.FindRef(TEXT("index")))));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == SlotSemanticId)
			{
				const UPanelSlot* Slot = Cast<UPanelSlot>(Request.RootObject);
				if (!Slot)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("slot_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildSlotState(*Slot));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == BindingSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint)
				{
					OutError = TEXT("umg_binding_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
				{
					if (Binding.ObjectName == Request.Context.FindRef(TEXT("widget_name"))
						&& Binding.PropertyName.ToString() == Request.Context.FindRef(TEXT("property_name")))
					{
						OutValue.Value = MakeShared<FJsonValueObject>(BuildBindingState(Binding));
						return UeAgentAuthoringState::EReadOutcome::Resolved;
					}
				}
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("binding_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == AnimationSemanticId)
			{
				const UWidgetAnimation* Animation = Cast<UWidgetAnimation>(Request.RootObject);
				if (!Animation)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("animation_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildAnimationState(*Animation));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			return UeAgentAuthoringState::EReadOutcome::Unavailable;
		}

		UClass* ResolveClass(const FString& ClassPath, UClass* RequiredBase)
		{
			UClass* Class = ClassPath.IsEmpty()
				? nullptr
				: StaticLoadClass(RequiredBase, nullptr, *ClassPath);
			return Class && Class->IsChildOf(RequiredBase) ? Class : nullptr;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;

			if (Request.TargetIdentity.TargetKind == TEXT("umg_asset"))
			{
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional, OutError);
				if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				OutValue.Value = MakeShared<FJsonValueObject>(BuildAssetVerificationState(
					*Seed, Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
			}
			else if (Request.TargetIdentity.TargetKind == TEXT("umg_widget"))
			{
				UClass* WidgetClass = ResolveClass(
					Request.TargetIdentity.GetSelector(TEXT("widget_class")), UWidget::StaticClass());
				if (!WidgetClass) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional, OutError);
				UWidget* Widget = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UWidget>(WidgetClass, NAME_None) : nullptr;
				if (!Widget)
				{
					OutError = TEXT("umg_widget_native_default_create_failed");
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildWidgetState(*Widget, FString(), INDEX_NONE));
			}
			else if (Request.TargetIdentity.TargetKind == TEXT("umg_slot"))
			{
				UClass* ParentClass = ResolveClass(Request.Context.FindRef(TEXT("parent_class")), UPanelWidget::StaticClass());
				UClass* WidgetClass = ResolveClass(
					Request.TargetIdentity.GetSelector(TEXT("widget_class")), UWidget::StaticClass());
				if (!ParentClass || !WidgetClass)
				{
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
				}
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional, OutError);
				UPanelWidget* Parent = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UPanelWidget>(ParentClass, NAME_None) : nullptr;
				UWidget* Child = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UWidget>(WidgetClass, NAME_None) : nullptr;
				UPanelSlot* Slot = Parent && Child ? Parent->AddChild(Child) : nullptr;
				if (!Slot)
				{
					OutError = TEXT("umg_slot_native_default_create_failed");
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildSlotState(*Slot));
			}
			else
			{
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			}
			OutValue.Provenance = NativeDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UWidgetBlueprint* CreateFactorySeed(
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		const EObjectFlags Flags,
		FString& OutError)
	{
		OutError.Reset();
		if (!Outer || !ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass())
			|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutError = TEXT("umg_factory_input_invalid");
			return nullptr;
		}
		if (Name.IsNone())
		{
			Name = MakeUniqueObjectName(Outer, UWidgetBlueprint::StaticClass(), TEXT("WidgetBlueprintSeed"));
		}
		UWidgetBlueprint* Result = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Outer,
			Name,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UeAgentInterface"))));
		if (!Result || !Result->WidgetTree)
		{
			OutError = TEXT("umg_factory_create_failed");
			return nullptr;
		}
		Result->SetFlags(Flags);
		return Result;
	}

	static bool AppendAssetContentReadRequests(
		UWidgetBlueprint& Asset,
		const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		if (!UeAgentBlueprintAuthoring::AppendReusableBlueprintExportSnapshotReadRequests(
			Asset, OutRequests, OutError, CanonicalAssetPath))
		{
			return false;
		}

		TMap<const UWidget*, FString> Parents;
		TMap<const UWidget*, int32> Indices;
		if (Asset.WidgetTree) BuildParentMaps(Asset.WidgetTree->RootWidget, FString(), Parents, Indices);
		TArray<UWidget*> Widgets;
		if (Asset.WidgetTree) CollectWidgets(Asset.WidgetTree->RootWidget, Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (!Widget) continue;
			const TMap<FString, FString> Selectors = WidgetSelectors(
				Asset, *Widget, CanonicalAssetPath);
			const TMap<FString, FString> Context = {
				{ TEXT("parent_name"), Parents.FindRef(Widget) },
				{ TEXT("index"), LexToString(Indices.FindRef(Widget)) },
			};
			if (!AddReadRequest(
				WidgetSemanticId, WidgetIdentityModelId, TEXT("umg_widget"),
				Selectors, Widget, Context, OutRequests, OutError)
				|| !AppendPropertyRequests(
					Widget,
					WidgetPropertySemanticId,
					WidgetPropertyIdentityModelId,
					TEXT("umg_widget_property"),
					Selectors,
					{ TEXT("Slot"), TEXT("bIsVariable") },
					OutRequests,
					OutError))
			{
				return false;
			}
			if (Widget->Slot
				&& (!AddReadRequest(
					SlotSemanticId, SlotIdentityModelId, TEXT("umg_slot"),
					Selectors, Widget->Slot, {}, OutRequests, OutError)
					|| !AppendPropertyRequests(
						Widget->Slot,
						SlotPropertySemanticId,
						SlotPropertyIdentityModelId,
						TEXT("umg_slot_property"),
						Selectors,
						{},
						OutRequests,
						OutError)))
			{
				return false;
			}
		}

		for (const FDelegateEditorBinding& Binding : Asset.Bindings)
		{
			const TMap<FString, FString> Selectors = {
				{ TEXT("asset_path"), CanonicalAssetPath },
				{ TEXT("widget_name"), Binding.ObjectName },
				{ TEXT("property_name"), Binding.PropertyName.ToString() },
			};
			if (!AddReadRequest(
				BindingSemanticId, BindingIdentityModelId, TEXT("umg_binding"),
				Selectors, &Asset, Selectors, OutRequests, OutError))
			{
				return false;
			}
		}

		for (UWidgetAnimation* Animation : Asset.Animations)
		{
			if (!Animation) continue;
			const TMap<FString, FString> Selectors = {
				{ TEXT("asset_path"), CanonicalAssetPath },
				{ TEXT("animation_name"), Animation->GetName() },
			};
			if (!AddReadRequest(
				AnimationSemanticId, AnimationIdentityModelId, TEXT("umg_animation"),
				Selectors, Animation, {}, OutRequests, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool BuildAssetSnapshotReadRequests(
		UWidgetBlueprint* Asset,
		const FString& CanonicalAssetPath,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty() || (!Asset && !bExpectedAbsent))
		{
			OutError = TEXT("umg_snapshot_identity_invalid");
			return false;
		}
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = AssetSemanticId;
		if (!BuildIdentity(
			AssetIdentityModelId,
			TEXT("umg_asset"),
			{ { TEXT("asset_path"), CanonicalAssetPath } },
			Request.TargetIdentity,
			OutError))
		{
			return false;
		}
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		Request.RootObject = Asset;
		Request.RootOwnerPath = CanonicalAssetPath;
		if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
		OutRequests.Add(MoveTemp(Request));
		return !Asset || AppendAssetContentReadRequests(
			*Asset, CanonicalAssetPath, OutRequests, OutError);
	}

	bool BuildAssetExportSnapshotReadRequests(
		UWidgetBlueprint& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetPath = Asset.GetOutermost()->GetName();
		const TMap<FString, FString> AssetSelectors = { { TEXT("asset_path"), AssetPath } };
		if (!AddReadRequest(
			ExportTargetSemanticId, AssetIdentityModelId, TEXT("umg_asset"),
			AssetSelectors, &Asset, {}, OutRequests, OutError)
			|| !AddReadRequest(
				ParentClassSemanticId, AssetIdentityModelId, TEXT("umg_asset"),
				AssetSelectors, &Asset, {}, OutRequests, OutError))
		{
			return false;
		}

		return AppendAssetContentReadRequests(
			Asset, AssetPath, OutRequests, OutError);
	}

	bool RegisterStateAccessor(FString& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("umg_asset"), TEXT("umg_widget"), TEXT("umg_slot"),
			TEXT("umg_binding"), TEXT("umg_animation")
		};
		Descriptor.CapabilityTags = {
			TEXT("asset"), TEXT("widget_tree"), TEXT("slot"),
			TEXT("layout"), TEXT("binding"), TEXT("animation")
		};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(FString& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = NativeDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative;
		Descriptor.TargetKinds = { TEXT("umg_asset"), TEXT("umg_widget"), TEXT("umg_slot") };
		Descriptor.CapabilityTags = {
			TEXT("factory"), TEXT("widget_tree"), TEXT("panel_slot"), TEXT("post_create")
		};
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}
