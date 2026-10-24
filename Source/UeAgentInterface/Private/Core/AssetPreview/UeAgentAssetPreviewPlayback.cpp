#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/AssetPreview/UeAgentAssetPreviewTimeline.h"

#include "AnimPreviewInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorViewportClient.h"
#include "EditorViewportCommands.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Commands/InputBindingManager.h"
#include "IPersonaPreviewScene.h"
#include "ISequencer.h"
#include "PaperFlipbookComponent.h"
#include "PaperFlipbook.h"
#include "SEditorViewport.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		bool SupportsNativeFocus(const FContext& Context)
		{
			static const TSet<FName> Types = {TEXT("SMaterialEditor3DPreviewViewport"),
				TEXT("SAnimationEditorViewport"), TEXT("SNiagaraSystemViewport"),
				TEXT("SSpriteEditorViewport"), TEXT("SFlipbookEditorViewport"),
				TEXT("STileSetSelectorViewport"), TEXT("SSingleTileEditorViewport"),
				TEXT("SSCSEditorViewport"), TEXT("SStaticMeshEditorViewport")};
			return Types.Contains(Context.ViewportType);
		}
		IPersonaPreviewScene* PersonaScene(const FContext& Context)
		{
			if (Context.ViewportType != TEXT("SAnimationEditorViewport") || !Context.AdvancedScene) return nullptr;
			return static_cast<IPersonaPreviewScene*>(Context.AdvancedScene);
		}
		float PersonaRate(const IPersonaPreviewScene& Scene)
		{
			const auto* World = Scene.GetWorld();
			return World ? World->GetWorldSettings()->TimeDilation : Scene.GetCustomAnimationSpeed();
		}
		UAnimPreviewInstance* AnimationPlayer(const FContext& Context)
		{
			auto* Scene = PersonaScene(Context);
			if (!Scene) return nullptr;
			auto* Mesh = Scene->GetPreviewMeshComponent();
			return Mesh && Mesh->IsPreviewOn() ? Mesh->PreviewInstance.Get() : nullptr;
		}

		UPaperFlipbookComponent* FlipbookPlayer(const FContext& Context)
		{
			if (Context.ViewportType != TEXT("SFlipbookEditorViewport") || !Context.Viewport) return nullptr;
			const UWorld* World = Context.Viewport->GetViewportClient()->GetWorld();
			UPaperFlipbookComponent* Found = nullptr;
			for (TObjectIterator<UPaperFlipbookComponent> It; It; ++It)
			{
				if (!It->IsRegistered() || It->GetWorld() != World) continue;
				if (Found) return nullptr;
				Found = *It;
			}
			return Found;
		}
	}

	void RefreshSkeletalPreviewPose(UDebugSkelMeshComponent& Mesh)
	{
		// Changing a paused player's position only updates its cursor. Complete
		// evaluation synchronously before the viewport consumes the new pose.
		Mesh.TickAnimation(0.0f, false);
		Mesh.RefreshBoneTransforms(nullptr);
		Mesh.RefreshFollowerComponents();
		Mesh.UpdateComponentToWorld();
		Mesh.UpdateBounds();
		Mesh.MarkRenderTransformDirty();
		Mesh.MarkRenderDynamicDataDirty();
	}

	TOptional<FFrameRate> PreviewFrameRate(const FContext& Context)
	{
		if (const auto Sequencer = FindPreviewSequencer(Context.Asset.Get())) return Sequencer->GetFocusedDisplayRate();
		if (auto* Player = AnimationPlayer(Context))
			if (const auto* Sequence = Cast<UAnimSequenceBase>(Player->GetCurrentAsset())) return Sequence->GetSamplingFrameRate();
		if (auto* Player = FlipbookPlayer(Context); Player && Player->GetFlipbook())
			return FFrameRate(FMath::RoundToInt(Player->GetFlipbook()->GetFramesPerSecond() * 1000000.), 1000000);
		return {};
	}

	bool ReadPlayback(const FContext& Context, FUeAgentAssetPreviewPlayback& Out)
	{
		if (auto Sequencer = FindPreviewSequencer(Context.Asset.Get()))
		{
			Out.Rate = Sequencer->GetPlaybackSpeed();
			return true;
		}
		if (auto* Scene = PersonaScene(Context); Scene && Scene->GetPreviewMeshComponent())
		{
			if (Scene->IsShowReferencePoseEnabled()) return false;
			Out.Rate = PersonaRate(*Scene);
			if (auto* Player = AnimationPlayer(Context))
			{
				Out.Loop = Player->IsLooping();
				if (const auto* Montage = Cast<UAnimMontage>(Player->GetCurrentAsset()))
				{
					Out.MontageMode = Player->MontagePreviewType == EMPT_AllSections ? EUeAgentAssetPreviewMontageMode::AllSections : EUeAgentAssetPreviewMontageMode::Sections;
					Out.StartSection = Montage->GetSectionName(Player->MontagePreviewStartSectionIdx);
				}
				if (Cast<UBlendSpace>(Player->GetCurrentAsset()))
				{
					FVector Input, Filtered;
					Player->GetBlendSpaceState(Input, Filtered);
					Out.BlendInputX = Input.X;
					Out.BlendInputY = Input.Y;
				}
			}
			return true;
		}
		if (auto* Player = FlipbookPlayer(Context))
		{
			Out.Rate = Player->GetPlayRate();
			Out.Loop = Player->IsLooping();
			return true;
		}
		return false;
	}

	bool IsPlaybackFieldSupported(const FContext& Context, FName Field)
	{
		const auto* Player = AnimationPlayer(Context);
		const auto* Asset = Player ? Player->GetAnimationAsset() : nullptr;
		if (Field == TEXT("MontageMode") || Field == TEXT("StartSection")) return Asset && Asset->IsA<UAnimMontage>();
		if (Field == TEXT("BlendInputX") || Field == TEXT("BlendInputY"))
			return Asset && Asset->IsA<UBlendSpace>() && (Field == TEXT("BlendInputX") || !Asset->IsA<UBlendSpace1D>());
		return Field != TEXT("Loop") || AnimationPlayer(Context) || FlipbookPlayer(Context);
	}

	TArray<FString> GetActions(UObject* Asset, const FString& Target)
	{
		if (IsAudioPreview(Asset)) return {TEXT("status"), TEXT("play"), TEXT("pause"), TEXT("stop")};
		TArray<FString> Actions = {TEXT("status")};
		FContext Context;
		UE::UAI::Contract::FDiagnostic Error;
		if (!ResolveContext(Asset, false, Context, Error, Target)) return Actions;
		if (SupportsNativeFocus(Context)) Actions.Add(TEXT("focus"));
		if (Context.ViewportType == TEXT("SSCSEditorViewport")) Actions.Add(TEXT("reset_view"));
		if (IsPhysicsPreview(Asset)) { Actions.Append({TEXT("play"), TEXT("stop")}); return Actions; }
		FUeAgentAssetPreviewPlayback Playback;
		if (ReadPlayback(Context, Playback))
		{
			Actions.Append({TEXT("play"), TEXT("pause")});
			if (!PersonaScene(Context) || AnimationPlayer(Context)) Actions.Append({TEXT("reset"), TEXT("seek")});
		}
		else if (Context.ViewportType == TEXT("SNiagaraSystemViewport")) Actions.Add(TEXT("reset"));
		return Actions;
	}

	bool ReconcilePlaybackSelection(const FContext& Context, FUeAgentAssetPreviewPlayback& Value)
	{
		auto* Player = AnimationPlayer(Context);
		const auto* Montage = Player ? Cast<UAnimMontage>(Player->GetCurrentAsset()) : nullptr;
		if (!Montage || Value.StartSection.IsNone() || Montage->GetSectionIndex(Value.StartSection) != INDEX_NONE)
			return false;
		FUeAgentAssetPreviewPlayback Current;
		if (!ReadPlayback(Context, Current)) return false;
		Value.StartSection = Current.StartSection;
		return true;
	}

	bool ApplyPlayback(const FContext& Context, const FUeAgentAssetPreviewPlayback& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto FailValue = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Field,
			TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual)
		{
			OutError = PreviewValueFailure(Code, Reason, Field, MoveTemp(Expected), MoveTemp(Actual), Context.Asset.Get(), Context.Target);
			OutError.Stage = TEXT("preview_apply");
			return false;
		};
		if (!FMath::IsFinite(Value.Rate)) return FailValue(TEXT("preview_playback_rate_invalid"), TEXT("The preview playback rate must be finite."), TEXT("Rate"),
			MakeShared<FJsonValueString>(TEXT("finite playback rate")), PreviewNumber(Value.Rate));
		if (auto Sequencer = FindPreviewSequencer(Context.Asset.Get()))
		{
			if (Fields.Contains(TEXT("Loop")))
				return FailValue(TEXT("preview_loop_is_sequencer_settings_scope"), TEXT("Looping for a sequence is controlled by Sequencer settings, not the asset preview playback properties."), TEXT("Loop"),
					MakeShared<FJsonValueString>(TEXT("Sequencer loop settings")), MakeShared<FJsonValueBoolean>(Value.Loop));
			if (Fields.Contains(TEXT("Rate"))) Sequencer->SetPlaybackSpeed(Value.Rate);
			return true;
		}
		if (auto* Scene = PersonaScene(Context))
		{
			if (Value.Rate < 0) return FailValue(TEXT("persona_preview_rate_negative"), TEXT("The animation preview playback rate cannot be negative."), TEXT("Rate"),
				MakeShared<FJsonValueString>(TEXT("rate >= 0")), PreviewNumber(Value.Rate));
			auto* Player = AnimationPlayer(Context);
			auto* Montage = Player ? Cast<UAnimMontage>(Player->GetCurrentAsset()) : nullptr;
			const bool bMontageSelection = Fields.Contains(TEXT("MontageMode")) || Fields.Contains(TEXT("StartSection"));
			const bool bBlendInput = Fields.Contains(TEXT("BlendInputX")) || Fields.Contains(TEXT("BlendInputY"));
			if (bMontageSelection && !Montage)
				return FailValue(TEXT("preview_montage_section_unavailable"), TEXT("Montage playback selection requires an active animation montage preview player."),
					Fields.Contains(TEXT("StartSection")) ? TEXT("StartSection") : TEXT("MontageMode"),
					MakeShared<FJsonValueString>(TEXT("animation montage preview player")), MakeShared<FJsonValueString>(Player ? GetPathNameSafe(Player->GetCurrentAsset()) : TEXT("no animation preview player")));
			if (bMontageSelection && !Value.StartSection.IsNone() && Montage->GetSectionIndex(Value.StartSection) == INDEX_NONE)
			{
				TArray<TSharedPtr<FJsonValue>> Sections;
				for (int32 Index = 0; Index < Montage->GetNumSections(); ++Index) Sections.Add(MakeShared<FJsonValueString>(Montage->GetSectionName(Index).ToString()));
				return FailValue(TEXT("preview_montage_section_unavailable"), TEXT("The requested start section does not exist in the preview montage."), TEXT("StartSection"),
					MakeShared<FJsonValueArray>(MoveTemp(Sections)), MakeShared<FJsonValueString>(Value.StartSection.ToString()));
			}
			if (bBlendInput && (!FMath::IsFinite(Value.BlendInputX) || !FMath::IsFinite(Value.BlendInputY)))
			{
				const bool bInvalidX = !FMath::IsFinite(Value.BlendInputX);
				return FailValue(TEXT("preview_blend_input_invalid"), TEXT("The preview blend input must be finite."), bInvalidX ? TEXT("BlendInputX") : TEXT("BlendInputY"),
					MakeShared<FJsonValueString>(TEXT("finite blend input")), PreviewNumber(bInvalidX ? Value.BlendInputX : Value.BlendInputY));
			}
			if (bBlendInput && (!Player || !Cast<UBlendSpace>(Player->GetCurrentAsset())))
				return FailValue(TEXT("preview_blend_input_invalid"), TEXT("Blend inputs require an active BlendSpace preview player."),
					Fields.Contains(TEXT("BlendInputX")) ? TEXT("BlendInputX") : TEXT("BlendInputY"), MakeShared<FJsonValueString>(TEXT("BlendSpace preview player")),
					MakeShared<FJsonValueString>(Player ? GetPathNameSafe(Player->GetCurrentAsset()) : TEXT("no animation preview player")));
			if (bMontageSelection)
			{
				const bool bPlaying = Player->IsPlaying();
				if (Value.MontageMode == EUeAgentAssetPreviewMontageMode::AllSections) Player->MontagePreview_PreviewAllSections(bPlaying);
				else Player->MontagePreview_PreviewNormal(Value.StartSection.IsNone() ? 0 : Montage->GetSectionIndex(Value.StartSection), bPlaying);
			}
			if (bBlendInput)
			{
				FVector Input, Filtered;
				Player->GetBlendSpaceState(Input, Filtered);
				if (Fields.Contains(TEXT("BlendInputX"))) Input.X = Value.BlendInputX;
				if (Fields.Contains(TEXT("BlendInputY"))) Input.Y = Value.BlendInputY;
				Player->SetBlendSpacePosition(Input);
			}
			if (Fields.Contains(TEXT("Rate"))) Scene->SetCustomAnimationSpeed(Value.Rate);
			if (Fields.Contains(TEXT("Loop")))
			{
				if (!Player) return FailValue(TEXT("persona_preview_loop_unavailable"), TEXT("Looping cannot be applied because the animation preview has no active player."), TEXT("Loop"),
					MakeShared<FJsonValueString>(TEXT("active animation preview player")), MakeShared<FJsonValueBoolean>(Value.Loop));
				if (Montage) Player->MontagePreview_SetLooping(Value.Loop);
				else Player->SetLooping(Value.Loop);
			}
			if (bMontageSelection || bBlendInput)
			{
				if (auto* Mesh = Scene->GetPreviewMeshComponent()) RefreshSkeletalPreviewPose(*Mesh);
				Scene->InvalidateViews();
			}
			return true;
		}
		if (auto* Player = FlipbookPlayer(Context))
		{
			if (Fields.Contains(TEXT("Rate"))) Player->SetPlayRate(Value.Rate);
			if (Fields.Contains(TEXT("Loop"))) Player->SetLooping(Value.Loop);
			return true;
		}
		OutError = PreviewFailure(TEXT("preview_playback_owner_unavailable"), TEXT("The preview has no supported native player to apply playback settings."), Context.Asset.Get(), Context.Target);
		OutError.ActualValue = MakeShared<FJsonValueString>(Context.ViewportType.ToString());
		return false;
	}

	bool ExecuteAction(UObject* Asset, const FString& Target, const FString& Action,
		TOptional<double> TimeSeconds, TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError,
		bool bPreservePlaybackRate)
	{
		if (TimeSeconds.IsSet() != (Action == TEXT("seek"))
			|| (TimeSeconds.IsSet() && !FMath::IsFinite(TimeSeconds.GetValue())))
		{
			OutError = PreviewValueFailure(TEXT("preview_seek_time_invalid"), !TimeSeconds.IsSet() ? TEXT("The seek action requires a time in seconds.")
				: Action != TEXT("seek") ? TEXT("A time in seconds can only be supplied for the seek action.") : TEXT("The seek time must be finite."), TEXT("time_seconds"),
				MakeShared<FJsonValueString>(TEXT("finite seconds for seek only")), TimeSeconds.IsSet() ? PreviewNumber(TimeSeconds.GetValue()) : nullptr, Asset, Target);
			OutError.SourceKind = TEXT("command_input"); OutError.ExpectedValuePresent = Action == TEXT("seek"); OutError.ActualValuePresent = TimeSeconds.IsSet(); return false;
		}
		FContext Context;
		if (!ResolveContext(Asset, Action != TEXT("status"), Context, OutError, Target)) return false;
		Asset = Context.Asset.Get();
		if (IsAudioPreview(Asset)) return ExecuteAudioAction(Asset, Action, OutDetails, OutError);
		if (Action == TEXT("focus") || Action == TEXT("reset_view"))
		{
			const auto Commands = Context.Viewport ? Context.Viewport->GetCommandList() : nullptr;
			const auto Command = Action == TEXT("reset_view")
				? (Context.ViewportType == TEXT("SSCSEditorViewport")
					? FInputBindingManager::Get().FindCommandInContext(TEXT("BlueprintEditor"), TEXT("ResetCamera")) : nullptr)
				: FEditorViewportCommands::Get().FocusViewportToSelection;
			if (!SupportsNativeFocus(Context) || !Commands || !Command
				|| !Commands->TryExecuteAction(Command.ToSharedRef()))
			{
				OutError = PreviewFailure(TEXT("preview_focus_unavailable"), !SupportsNativeFocus(Context) ? TEXT("The preview viewport does not support native focus or view reset.")
					: !Commands ? TEXT("The preview viewport has no native command list.") : !Command ? TEXT("The requested view command is not registered for this preview viewport.")
					: TEXT("The native preview viewport rejected the requested view command."), Asset, Context.Target);
				OutError.PropertyPath = TEXT("action"); OutError.ActualValue = MakeShared<FJsonValueString>(Action);
				OutError.InternalContext = Context.ViewportType.ToString(); return false;
			}
			OutDetails = MakeShared<FJsonObject>();
			OutDetails->SetStringField(TEXT("target"), Context.Target);
			OutDetails->SetStringField(TEXT("action"), Action);
			OutDetails->SetStringField(TEXT("state"), Action == TEXT("reset_view") ? TEXT("native_view_reset") : TEXT("native_focus_requested"));
			return true;
		}
		if (IsPhysicsPreview(Asset)) return ExecuteSimulationAction(Asset, Action, OutDetails, OutError);
		if (Action != TEXT("status") && Action != TEXT("play") && Action != TEXT("pause")
			&& Action != TEXT("reset") && Action != TEXT("seek"))
		{ OutError = PreviewValueFailure(TEXT("preview_action_unknown"), TEXT("The action is not supported by this preview player."), TEXT("action"), MakeShared<FJsonValueString>(TEXT("status, play, pause, reset or seek")), MakeShared<FJsonValueString>(Action), Asset, Context.Target); OutError.SourceKind = TEXT("command_input"); return false; }
		OutDetails = MakeShared<FJsonObject>();
		OutDetails->SetStringField(TEXT("target"), Context.Target);
		OutDetails->SetStringField(TEXT("source"), TEXT("live"));
		OutDetails->SetStringField(TEXT("action"), Action);
		ReadTimelineStatus(Context, *OutDetails);
		if (Context.ImageViewport) ReadImageStatus(Context, *OutDetails);
		if (Context.bWidgetPreview) ReadWidgetStatus(Context, *OutDetails);
		if (const auto* Scene = PersonaScene(Context); Scene && Scene->IsShowReferencePoseEnabled())
		{
			if (Action != TEXT("status")) { OutError = PreviewValueFailure(TEXT("preview_reference_pose_has_no_animation"), TEXT("The preview is showing the reference pose and has no animation player for this action."), TEXT("action"), MakeShared<FJsonValueString>(TEXT("status")), MakeShared<FJsonValueString>(Action), Asset, Context.Target); OutError.SourceKind = TEXT("native_result"); return false; }
			OutDetails->SetBoolField(TEXT("playback_available"), false);
			OutDetails->SetStringField(TEXT("pose_source"), TEXT("reference_pose"));
			return true;
		}
		if (Context.ViewportType == TEXT("SNiagaraSystemViewport") || Context.ViewportType == TEXT("SLevelViewport"))
		{
			if (Action == TEXT("reset") && Context.ViewportType == TEXT("SNiagaraSystemViewport") && !ResetNiagaraPreview(Asset, OutError)) return false;
			const auto Sequencer = FindPreviewSequencer(Asset);
			if (!Sequencer)
			{
				if (Action != TEXT("status") && Action != TEXT("reset"))
				{ OutError = PreviewFailure(TEXT("preview_sequencer_owner_unavailable_for_existing_editor"), TEXT("The existing preview editor has no registered sequencer to execute the requested playback action."), Asset, Context.Target); OutError.ActualValue = MakeShared<FJsonValueString>(Action); return false; }
				OutDetails->SetBoolField(TEXT("playback_available"), false);
				OutDetails->SetStringField(TEXT("playback_unavailable_reason"), TEXT("existing_editor_sequencer_not_registered"));
				return true;
			}
			const float PreviousRate = Sequencer->GetPlaybackSpeed();
			if (Action == TEXT("play")) Sequencer->OnPlay(false);
			else if (Action == TEXT("pause")) Sequencer->Pause();
			else if (Action == TEXT("seek") || (Action == TEXT("reset") && Context.ViewportType == TEXT("SLevelViewport")))
				Sequencer->SetLocalTimeDirectly(FFrameTime::FromDecimal(TimeSeconds.Get(0) * Sequencer->GetFocusedTickResolution().AsDecimal()));
			// Capture owns only a temporary playback transition. Native Pause restores
			// the speed from before Play, which must not replace the workspace setting.
			if (bPreservePlaybackRate && Sequencer->GetPlaybackSpeed() != PreviousRate)
				Sequencer->SetPlaybackSpeed(PreviousRate);
			OutDetails->SetStringField(TEXT("preview_source"), Context.Source);
			OutDetails->SetBoolField(TEXT("playback_available"), true);
			OutDetails->SetStringField(TEXT("time_basis"), TEXT("focused_sequence_local"));
			OutDetails->SetBoolField(TEXT("playing"), Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing);
			OutDetails->SetNumberField(TEXT("time_seconds"), Sequencer->GetLocalTime().AsSeconds());
			OutDetails->SetNumberField(TEXT("rate"), Sequencer->GetPlaybackSpeed());
			ReadTimelineStatus(Context, *OutDetails);
			return true;
		}
		bool bPlaying = false;
		double Position = 0, Duration = 0;
		if (auto* Player = AnimationPlayer(Context))
		{
			Duration = Player->GetLength();
			if (TimeSeconds.IsSet() && (TimeSeconds.GetValue() < 0 || TimeSeconds.GetValue() > Duration))
			{ OutError = PreviewValueFailure(TEXT("preview_seek_out_of_range"), TEXT("The seek time is outside the animation duration."), TEXT("time_seconds"), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= seconds <= %.17g"), Duration)), PreviewNumber(TimeSeconds.GetValue()), Asset, Context.Target); OutError.SourceKind = TEXT("command_input"); return false; }
			const auto* Montage = Cast<UAnimMontage>(Player->GetCurrentAsset());
			if (Action == TEXT("play")) { if (Montage) Player->MontagePreview_SetPlaying(true); else Player->SetPlaying(true); }
			else if (Action == TEXT("pause")) { if (Montage) Player->MontagePreview_SetPlaying(false); else Player->SetPlaying(false); }
			else if (Action == TEXT("seek") || Action == TEXT("reset"))
			{
				// Seeking preview state must not dispatch gameplay notifies.
				if (Montage) Player->MontagePreview_JumpToPosition(static_cast<float>(TimeSeconds.Get(0)));
				else Player->SetPosition(static_cast<float>(TimeSeconds.Get(0)), false);
				if (auto* Scene = PersonaScene(Context))
					if (auto* Mesh = Scene->GetPreviewMeshComponent()) RefreshSkeletalPreviewPose(*Mesh);
			}
			Position = Player->GetCurrentTime();
			bPlaying = Player->IsPlaying();
			if (Montage)
			{
				OutDetails->SetStringField(TEXT("montage_mode"), Player->MontagePreviewType == EMPT_AllSections ? TEXT("all_sections") : TEXT("sections"));
				OutDetails->SetStringField(TEXT("start_section"), Montage->GetSectionName(Player->MontagePreviewStartSectionIdx).ToString());
				OutDetails->SetStringField(TEXT("current_section"), Montage->GetSectionName(Montage->GetSectionIndexFromPosition(Position)).ToString());
				OutDetails->SetBoolField(TEXT("slot_readback_available"), false);
				TArray<TSharedPtr<FJsonValue>> Sections;
				for (const auto& Section : Montage->CompositeSections) Sections.Add(MakeShared<FJsonValueString>(Section.SectionName.ToString()));
				OutDetails->SetArrayField(TEXT("sections"), Sections);
			}
			if (const auto* Blend = Cast<UBlendSpace>(Player->GetCurrentAsset()))
			{
				FVector Input, Filtered;
				Player->GetBlendSpaceState(Input, Filtered);
				TArray<TSharedPtr<FJsonValue>> Axes;
				for (int32 Index = 0; Index < (Blend->IsA<UBlendSpace1D>() ? 1 : 2); ++Index)
				{
					const auto& Parameter = Blend->GetBlendParameter(Index);
					auto Axis = MakeShared<FJsonObject>();
					Axis->SetStringField(TEXT("field"), Index == 0 ? TEXT("BlendInputX") : TEXT("BlendInputY"));
					Axis->SetStringField(TEXT("name"), Parameter.DisplayName);
					Axis->SetNumberField(TEXT("minimum"), Parameter.Min);
					Axis->SetNumberField(TEXT("maximum"), Parameter.Max);
					Axis->SetNumberField(TEXT("input"), Input[Index]);
					Axis->SetNumberField(TEXT("filtered_input"), Filtered[Index]);
					Axes.Add(MakeShared<FJsonValueObject>(Axis));
				}
				OutDetails->SetArrayField(TEXT("blend_axes"), Axes);
			}
		}
		else if (auto* Flipbook = FlipbookPlayer(Context))
		{
			Duration = Flipbook->GetFlipbookLength();
			if (TimeSeconds.IsSet() && (TimeSeconds.GetValue() < 0 || TimeSeconds.GetValue() > Duration))
			{ OutError = PreviewValueFailure(TEXT("preview_seek_out_of_range"), TEXT("The seek time is outside the flipbook duration."), TEXT("time_seconds"), MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= seconds <= %.17g"), Duration)), PreviewNumber(TimeSeconds.GetValue()), Asset, Context.Target); OutError.SourceKind = TEXT("command_input"); return false; }
			if (Action == TEXT("play")) Flipbook->Play();
			else if (Action == TEXT("pause")) Flipbook->Stop();
			else if (Action == TEXT("seek") || Action == TEXT("reset"))
				Flipbook->SetPlaybackPosition(static_cast<float>(TimeSeconds.Get(0)), false);
			Position = Flipbook->GetPlaybackPosition();
			bPlaying = Flipbook->IsPlaying();
		}
		else if (auto* Scene = PersonaScene(Context); Scene && Scene->GetPreviewMeshComponent())
		{
			auto* Mesh = Scene->GetPreviewMeshComponent();
			if ((Action == TEXT("play") && Mesh->GlobalAnimRateScale <= 0)
				|| (Action == TEXT("pause") && Mesh->GlobalAnimRateScale > 0)) Scene->TogglePlayback();
			else if (Action != TEXT("play") && Action != TEXT("pause") && Action != TEXT("status"))
			{ OutError = PreviewValueFailure(TEXT("persona_preview_timeline_unavailable"), TEXT("This Persona preview uses the scene clock and has no seekable asset timeline."), TEXT("action"), MakeShared<FJsonValueString>(TEXT("play, pause or status")), MakeShared<FJsonValueString>(Action), Asset, Context.Target); OutError.SourceKind = TEXT("native_result"); return false; }
			OutDetails->SetBoolField(TEXT("playback_available"), true);
			OutDetails->SetBoolField(TEXT("playing"), Mesh->GlobalAnimRateScale > 0);
			OutDetails->SetBoolField(TEXT("position_available"), false);
			OutDetails->SetStringField(TEXT("time_basis"), TEXT("preview_scene_clock"));
			OutDetails->SetNumberField(TEXT("rate"), PersonaRate(*Scene));
			return true;
		}
		else
		{
			if (Action != TEXT("status")) { OutError = PreviewFailure(TEXT("preview_playback_owner_unavailable"), TEXT("The preview has no supported native player to execute this action."), Asset, Context.Target); OutError.ActualValue = MakeShared<FJsonValueString>(Action); return false; }
			OutDetails->SetBoolField(TEXT("playback_available"), false);
			return true;
		}
		OutDetails->SetBoolField(TEXT("playback_available"), true);
		OutDetails->SetStringField(TEXT("time_basis"), TEXT("asset_time"));
		OutDetails->SetBoolField(TEXT("playing"), bPlaying);
		OutDetails->SetNumberField(TEXT("time_seconds"), Position);
		OutDetails->SetNumberField(TEXT("duration_seconds"), Duration);
		Context.Viewport->GetViewportClient()->Invalidate();
		return true;
	}
}
