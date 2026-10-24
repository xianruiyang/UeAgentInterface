#include "Core/AssetPreview/UeAgentAssetPreviewTimeline.h"

#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISequencer.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "SEditorViewport.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		UNiagaraComponent* PreviewSimulation(const FContext& Context)
		{
			if (Context.ViewportType != TEXT("SNiagaraSystemViewport") || !Context.Viewport) return nullptr;
			const auto* World = Context.Viewport->GetViewportClient()->GetWorld();
			UNiagaraComponent* Found = nullptr;
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
			{
				if (!It->IsRegistered() || It->GetWorld() != World) continue;
				if (Found) return nullptr;
				Found = *It;
			}
			if (const auto* System = Cast<UNiagaraSystem>(Context.Asset.Get()); System && Found && Found->GetAsset() != System) return nullptr;
			return Found;
		}
	}

	void ReadTimelineStatus(const FContext& Context, FJsonObject& Out)
	{
		if (const auto Client = Context.Viewport ? Context.Viewport->GetViewportClient() : nullptr)
		{
			Out.SetBoolField(TEXT("viewport_client_visible"), Client->IsVisible());
			Out.SetBoolField(TEXT("viewport_realtime"), Client->IsRealtime());
			if (const auto* World = Client->GetWorld())
				Out.SetNumberField(TEXT("preview_world_time_seconds"), World->GetTimeSeconds());
		}
		if (const auto Rate = PreviewFrameRate(Context); Rate.IsSet())
		{
			Out.SetNumberField(TEXT("frame_rate_numerator"), Rate->Numerator);
			Out.SetNumberField(TEXT("frame_rate_denominator"), Rate->Denominator);
		}
		if (const auto Seq = FindPreviewSequencer(Context.Asset.Get()))
		{
			Out.SetStringField(TEXT("capture_time_basis"), TEXT("focused_sequence_local"));
			Out.SetNumberField(TEXT("local_time_seconds"), Seq->GetLocalTime().AsSeconds());
			Out.SetNumberField(TEXT("actual_frame"), Seq->GetLocalTime().ConvertTo(Seq->GetFocusedDisplayRate()).AsDecimal());
			Out.SetStringField(TEXT("event_scope"), TEXT("native_editor_preview"));
			const auto Range = Seq->GetPlaybackRange();
			if (Range.HasLowerBound()) Out.SetNumberField(TEXT("playback_start_seconds"), Seq->GetFocusedTickResolution().AsSeconds(Range.GetLowerBoundValue()));
			if (Range.HasUpperBound()) Out.SetNumberField(TEXT("playback_end_seconds_exclusive"), Seq->GetFocusedTickResolution().AsSeconds(Range.GetUpperBoundValue()));
		}
		if (const auto* Component = PreviewSimulation(Context))
		{
			Out.SetBoolField(TEXT("simulation_tick_enabled"), Component->IsComponentTickEnabled());
			Out.SetBoolField(TEXT("simulation_active"), Component->IsActive());
			Out.SetNumberField(TEXT("desired_age_seconds"), Component->GetDesiredAge());
			Out.SetBoolField(TEXT("simulation_has_scene_proxy"), Component->GetSceneProxy() != nullptr);
			Out.SetBoolField(TEXT("simulation_pso_precaching"), Component->IsPSOPrecaching());
			Out.SetBoolField(TEXT("simulation_paused"), Component->IsPaused());
			if (const auto Controller = Component->GetSystemInstanceController(); Controller && Controller->IsValid())
			{
				Out.SetNumberField(TEXT("simulation_age_seconds"), Controller->GetAge());
				Out.SetBoolField(TEXT("simulation_complete"), Controller->IsComplete());
			}
		}
	}

	bool FContinuousCapture::Begin(const FContext& Context, double StartSeconds, double EndSeconds, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Sequencer = FindPreviewSequencer(Context.Asset.Get());
		if (!Sequencer) { OutError = PreviewFailure(TEXT("preview_continuous_requires_native_sequencer"), TEXT("Continuous preview capture requires a registered native sequencer for this asset."), Context.Asset.Get(), Context.Target); return false; }
		const auto Range = Sequencer->GetPlaybackRange();
		const auto Rate = Sequencer->GetFocusedTickResolution();
		if (!Range.HasLowerBound() || !Range.HasUpperBound())
		{ OutError = PreviewFailure(TEXT("preview_continuous_playback_range_unbounded"), !Range.HasLowerBound() ? TEXT("The native sequencer playback range has no lower bound.") : TEXT("The native sequencer playback range has no upper bound."), Context.Asset.Get(), Context.Target); return false; }
		const double Origin = Rate.AsSeconds(Range.GetLowerBoundValue());
		const double End = Rate.AsSeconds(Range.GetUpperBoundValue());
		if (!FMath::IsFinite(StartSeconds) || !FMath::IsFinite(EndSeconds) || StartSeconds < Origin || EndSeconds < StartSeconds || EndSeconds >= End)
		{ OutError = PreviewFailure(TEXT("preview_continuous_range_invalid"), FString::Printf(TEXT("requested %.6f..%.6f, native range %.6f..%.6f (exclusive end)"), StartSeconds, EndSeconds, Origin, End), Context.Asset.Get(), Context.Target); OutError.SourceKind = TEXT("command_input"); return false; }
		PreviousSpeed = Sequencer->GetPlaybackSpeed();
		bWasPlaying = Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing;
		bActive = true;
		if (Context.ViewportType == TEXT("SNiagaraSystemViewport") && Context.Viewport)
		{
			SimulationWorld = Context.Viewport->GetViewportClient()->GetWorld();
			WorldTickHandle = FWorldDelegates::OnWorldPostActorTick.AddLambda([this](UWorld* World, ELevelTick, float)
			{
				if (World == SimulationWorld.Get()) ++CompletedWorldTicks;
			});
		}
		Sequencer->Pause();
		Sequencer->SetPlaybackSpeed(1.f);
		if (Context.ViewportType == TEXT("SNiagaraSystemViewport") && !ResetNiagaraPreview(Context.Asset.Get(), OutError)) return false;
		// Only initialization relocates the cursor. Every subsequent interval is
		// evaluated by normal editor playback, including the interval before pause.
		Sequencer->SetLocalTimeDirectly(Range.GetLowerBoundValue());
		LastSeconds = Sequencer->GetLocalTime().AsSeconds();
		bExpectedPlaying = false;
		return true;
	}

	bool FContinuousCapture::Advance(const FContext& Context, double TargetSeconds, bool& bReady, UE::UAI::Contract::FDiagnostic& OutError)
	{
		bReady = false;
		if (!bActive || FindPreviewSequencer(Context.Asset.Get()) != Sequencer)
		{ OutError = PreviewFailure(TEXT("preview_continuous_owner_changed"), !bActive ? TEXT("Continuous capture is not active.") : TEXT("The asset's registered sequencer changed during continuous capture."), Context.Asset.Get(), Context.Target); return false; }
		const double Actual = Sequencer->GetLocalTime().AsSeconds();
		const bool Playing = Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing;
		if (!FMath::IsNearlyEqual(Sequencer->GetPlaybackSpeed(), 1.f) || (!bExpectedPlaying && Playing))
		{
			const bool bSpeedChanged = !FMath::IsNearlyEqual(Sequencer->GetPlaybackSpeed(), 1.f);
			OutError = PreviewFailure(TEXT("preview_continuous_playback_changed_externally"), bSpeedChanged ? TEXT("The sequencer playback speed changed during continuous capture.") : TEXT("The sequencer started playing while continuous capture expected it to remain paused."), Context.Asset.Get(), Context.Target);
			OutError.PropertyPath = bSpeedChanged ? TEXT("playback_speed") : TEXT("playing");
			OutError.ExpectedValue = bSpeedChanged ? PreviewNumber(1) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false));
			OutError.ActualValue = bSpeedChanged ? PreviewNumber(Sequencer->GetPlaybackSpeed()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Playing)); return false;
		}
		if (Actual + 1.e-6 < LastSeconds)
		{
			OutError = PreviewFailure(TEXT("preview_continuous_rewound_before_sample"), FString::Printf(TEXT("previous=%.9f actual=%.9f target=%.9f global=%.9f"), LastSeconds, Actual, TargetSeconds, Sequencer->GetGlobalTime().AsSeconds()), Context.Asset.Get(), Context.Target);
			return false;
		}
		LastSeconds = Actual;
		if (bSettling || Actual + 1.e-6 >= TargetSeconds)
		{
			if (!bSettling)
			{
				Sequencer->Pause();
				// Pause restores UE's pre-play speed; this temporary transition must
				// retain the capture clock, including a first sample before OnPlay.
				Sequencer->SetPlaybackSpeed(1.f);
				// Native Pause may snap a fractional position to a display frame.
				// This is our own transition, not an external rewind.
				LastSeconds = Sequencer->GetLocalTime().AsSeconds();
				bExpectedPlaying = false;
				bSettling = true;
				bSimulationCaughtUp = false;
				return true; // render the evaluated state before reading pixels
			}
			if (Context.ViewportType == TEXT("SNiagaraSystemViewport"))
			{
				const auto* Component = PreviewSimulation(Context);
				if (!Component)
				{ OutError = PreviewFailure(TEXT("preview_continuous_simulation_owner_unavailable"), TEXT("The Niagara preview simulation component disappeared before the capture sample settled."), Context.Asset.Get(), Context.Target); return false; }
				const auto Controller = Component->GetSystemInstanceController();
				if (!Controller || !Controller->IsValid()) return true;
				if (Component->IsPSOPrecaching() || !Component->GetSceneProxy()) return true;
				if (!Controller->IsComplete() && Controller->GetAge() + Component->GetSeekDelta() + 1.e-6 < Component->GetDesiredAge()) return true;
				// Native DesiredAge clears its seeking/render suppression during a
				// component tick after catching up, not during the GT capture ticker.
				if (!bSimulationCaughtUp)
				{
					ReadyAfterWorldTick = CompletedWorldTicks + 1;
					bSimulationCaughtUp = true;
					return true;
				}
				if (CompletedWorldTicks < ReadyAfterWorldTick) return true;
			}
			bSettling = false;
			bReady = true;
			return true;
		}
		if (bExpectedPlaying && !Playing)
		{ OutError = PreviewFailure(TEXT("preview_continuous_stopped_before_sample"), TEXT("Native playback stopped before reaching the requested sample time in seconds."), Context.Asset.Get(), Context.Target); OutError.ExpectedValue = PreviewNumber(TargetSeconds); OutError.ActualValue = PreviewNumber(Actual); return false; }
		if (!Playing) { Sequencer->OnPlay(false); bExpectedPlaying = true; }
		return true;
	}

	void FContinuousCapture::Restore()
	{
		if (!bActive) return;
		bActive = false;
		FWorldDelegates::OnWorldPostActorTick.Remove(WorldTickHandle);
		WorldTickHandle.Reset();
		if (!Sequencer) return;
		const float CurrentSpeed = Sequencer->GetPlaybackSpeed();
		const float RestoredSpeed = FMath::IsNearlyEqual(CurrentSpeed, 1.f) ? PreviousSpeed : CurrentSpeed;
		const bool Playing = Sequencer->GetPlaybackStatus() == EMovieScenePlayerStatus::Playing;
		if (Playing == bExpectedPlaying && Playing != bWasPlaying)
		{ if (bWasPlaying) Sequencer->OnPlay(false); else Sequencer->Pause(); }
		// Restore speed after native play/pause, which may rewrite it. Preserve an
		// externally changed speed rather than overwriting it with the old setting.
		if (Sequencer->GetPlaybackSpeed() != RestoredSpeed) Sequencer->SetPlaybackSpeed(RestoredSpeed);
	}
}
