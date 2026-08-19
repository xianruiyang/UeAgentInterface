// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"
#include "Commands/Level/UeAgentHttpServer_LevelOps.h"

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"

namespace UeAgentEditorViewport
{
	class FLevelViewportAdapter final : public IViewportAdapter
	{
	public:
		explicit FLevelViewportAdapter(const FUeAgentHttpServer& InServer)
			: Server(InServer)
		{
		}

		virtual FString GetName() const override
		{
			return TEXT("level");
		}

		virtual bool CanResolve(const FResolveRequest& Request) const override
		{
			return Request.Target.Scope.IsEmpty() || Request.Target.Scope == TEXT("level");
		}

		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
		{
			FString SelectionReason;
			FLevelEditorViewportClient* Client = GetRequestedOrPreferredClient(Request.Target, &SelectionReason);
			if (!Client)
			{
				OutError = TEXT("no_level_editor_viewport");
				return false;
			}

			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("level");
			const int32 ClientIndex = GetClientIndex(Client);
			OutViewport.Target.Viewport = OutViewport.Target.Viewport.IsEmpty() ? TEXT("auto") : OutViewport.Target.Viewport;
			OutViewport.Target.ViewportId = OutViewport.Target.ViewportId.IsEmpty()
				? (ClientIndex >= 0 ? FString::Printf(TEXT("level:%d"), ClientIndex) : TEXT("level:auto"))
				: OutViewport.Target.ViewportId;
			OutViewport.Target.ViewportRole = TEXT("primary");
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FLevelEditorViewportClient");
			OutViewport.ViewportClient = Client;
			return true;
		}

		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}

			OutState.Target = Viewport.Target;
			OutState.ClientClass = TEXT("FLevelEditorViewportClient");
			OutState.Adapter = GetName();
			OutState.Camera.Location = Client->GetViewLocation();
			OutState.Camera.Rotation = Client->GetViewRotation();
			OutState.Camera.Fov = Client->ViewFOV;
			OutState.Camera.Space = TEXT("editor_world");
			OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
			OutState.Camera.bValid = true;
			OutState.Capabilities.bCamera = true;
			OutState.Capabilities.bVisualOptions = true;
			OutState.Capabilities.bPreviewState = true;

			OutState.Capabilities.FocusModes = { TEXT("selection"), TEXT("actor"), TEXT("actors"), TEXT("custom_bounds"), TEXT("custom_point") };  OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("visualization_family"), TEXT("visualization_mode"), TEXT("grid"), TEXT("bounds"), TEXT("pivot"), TEXT("selection"), TEXT("mode_widgets"), TEXT("collision") };
			OutState.VisualOptions = MakeLevelVisualOptionsJson(Client);
			OutState.PreviewState = MakeShared<FJsonObject>();
			return true;
		}

		virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			Client->SetViewLocation(Camera.Location);
			Client->SetViewRotation(Camera.Rotation);
			Client->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			return true;
		}

		virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			if (!FocusObj.IsValid())
			{
				OutError = TEXT("missing_focus");
				return false;
			}

			FString Mode = TEXT("selection");
			TryGetStringField(FocusObj, TEXT("mode"), Mode);
			Mode = NormalizeToken(Mode);
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("mode"), Mode);

			if (Mode == TEXT("actor"))
			{
				FString Id;
				if (!TryGetStringField(FocusObj, TEXT("actor_id"), Id)
					&& !TryGetStringField(FocusObj, TEXT("id"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor_name"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor_label"), Id))
				{
					OutError = TEXT("missing_actor_id");
					return false;
				}

				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_actor");
				ProxyCtx.Command = TEXT("viewport_frame_actor");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				ProxyCtx.Params->SetStringField(TEXT("id"), Id);
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				bool bCollisionAware = false;
				if (TryGetBoolField(FocusObj, TEXT("collision_aware"), bCollisionAware))
				{
					ProxyCtx.Params->SetBoolField(TEXT("collision_aware"), bCollisionAware);
				}
				bool bLookAt = true;
				if (TryGetBoolField(FocusObj, TEXT("look_at"), bLookAt))
				{
					ProxyCtx.Params->SetBoolField(TEXT("look_at"), bLookAt);
				}
				CopyJsonFieldIfPresent(FocusObj, TEXT("auto_fallback"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_channel"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_complex"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("safety_offset_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_step_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_offsets_cm"), ProxyCtx.Params);
				return Server.CmdViewportFrameActorCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("selection"))
			{
				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_selection");
				ProxyCtx.Command = TEXT("viewport_frame_selection");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				bool bInstant = true;
				if (TryGetBoolField(FocusObj, TEXT("instant"), bInstant))
				{
					ProxyCtx.Params->SetBoolField(TEXT("instant"), bInstant);
				}
				return Server.CmdViewportFrameSelectionCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("actors"))
			{
				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_actors");
				ProxyCtx.Command = TEXT("viewport_frame_actors");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
				if (!FocusObj->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds)
				{
					OutError = TEXT("missing_actor_ids");
					return false;
				}
				ProxyCtx.Params->SetArrayField(TEXT("actor_ids"), *ActorIds);
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				CopyJsonFieldIfPresent(FocusObj, TEXT("instant"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("collision_aware"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("auto_fallback"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_channel"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_complex"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("safety_offset_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_step_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_offsets_cm"), ProxyCtx.Params);
				return Server.CmdViewportFrameActorsCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("custom_bounds"))
			{
				FVector Min = FVector::ZeroVector;
				FVector Max = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("min"), Min) || !TryReadVectorField(FocusObj, TEXT("max"), Max))
				{
					TSharedPtr<FJsonObject> BoundsObj;
					if (!TryGetObjectField(FocusObj, TEXT("bounds"), BoundsObj)
						|| !TryReadVectorField(BoundsObj, TEXT("min"), Min)
						|| !TryReadVectorField(BoundsObj, TEXT("max"), Max))
					{
						OutError = TEXT("missing_custom_bounds");
						return false;
					}
				}
				const FBox Box(Min, Max);
				if (!Box.IsValid)
				{
					OutError = TEXT("invalid_custom_bounds");
					return false;
				}
				bool bInstant = true;
				TryGetBoolField(FocusObj, TEXT("instant"), bInstant);
				Client->FocusViewportOnBox(Box, bInstant);
				Client->Invalidate();
				TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
				BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
				BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
				BoundsJson->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
				BoundsJson->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
				OutData->SetObjectField(TEXT("bounds"), BoundsJson);
				OutData->SetBoolField(TEXT("instant"), bInstant);
				return true;
			}

			if (Mode == TEXT("custom_point"))
			{
				FVector Point = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("point"), Point))
				{
					OutError = TEXT("missing_custom_point");
					return false;
				}
				double Radius = 50.0;
				TryGetNumberField(FocusObj, TEXT("radius"), Radius);
				const FBox Box(Point - FVector(Radius), Point + FVector(Radius));
				Client->FocusViewportOnBox(Box, true);
				Client->Invalidate();
				OutData->SetObjectField(TEXT("point"), MakeVectorJson(Point));
				OutData->SetNumberField(TEXT("radius"), Radius);
				return true;
			}

			OutError = TEXT("unsupported_focus_mode");
			return false;
		}

		virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			if (!Options.IsValid())
			{
				OutError = TEXT("missing_visual_options");
				return false;
			}

			bool bHasRequestedViewMode = false;
			UeAgentLevelOps::FResolvedViewportViewMode RequestedViewMode;
			TArray<FString> Unsupported;
			if (!UeAgentLevelOps::TryResolveRequestedViewportViewMode(Options, bHasRequestedViewMode, RequestedViewMode, OutError))
			{
				bool bIgnoreUnsupported = false;
				TryGetBoolField(Options, TEXT("ignore_unsupported"), bIgnoreUnsupported);
				if (!bIgnoreUnsupported)
				{
					OutError = TEXT("unsupported_visual_options");
					return false;
				}
				Unsupported.Add(TEXT("view_mode"));
				OutError.Reset();
				bHasRequestedViewMode = false;
			}
			if (bHasRequestedViewMode)
			{
				UeAgentLevelOps::ApplyResolvedViewportViewMode(Client, RequestedViewMode);
			}

			bool bRealtime = Client->IsRealtime();
			if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
			{
				Client->SetRealtime(bRealtime);
			}
			bool bGameView = Client->IsInGameView();
			if (TryGetBoolField(Options, TEXT("game_view"), bGameView))
			{
				Client->SetGameView(bGameView);
			}

			auto ApplyBoolFlag = [Client, Options](const TCHAR* Key, auto Setter)
			{
				bool bValue = false;
				if (TryReadBoolOption(Options, Key, bValue))
				{
					Setter(bValue);
				}
			};

			// SetGameView rewrites several show flags, so explicit overlays are applied last.
			ApplyBoolFlag(TEXT("grid"), [Client](bool bValue) { Client->EngineShowFlags.SetGrid(bValue); });
			ApplyBoolFlag(TEXT("bounds"), [Client](bool bValue) { Client->EngineShowFlags.SetBounds(bValue); });
			ApplyBoolFlag(TEXT("pivot"), [Client](bool bValue) { Client->EngineShowFlags.SetPivot(bValue); });
			ApplyBoolFlag(TEXT("selection"), [Client](bool bValue) { Client->EngineShowFlags.SetSelection(bValue); });
			ApplyBoolFlag(TEXT("mode_widgets"), [Client](bool bValue) { Client->EngineShowFlags.SetModeWidgets(bValue); });
			ApplyBoolFlag(TEXT("collision"), [Client](bool bValue) { Client->EngineShowFlags.SetCollision(bValue); });

			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}

			OutData = MakeLevelVisualOptionsJson(Client);
			if (bHasRequestedViewMode)
			{
				OutData->SetObjectField(TEXT("requested_view_mode"), UeAgentLevelOps::MakeResolvedViewportViewModeJson(RequestedViewMode));
			}
			if (Unsupported.Num() > 0)
			{
				OutData->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Unsupported));
			}
			return true;
		}

		virtual bool ApplyPreviewState(const FResolvedViewport&, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			OutData = MakeShared<FJsonObject>();
			if (!HasUserFields(PreviewState))
			{
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
				return true;
			}

			FString ActorId;
			if (!TryGetStringField(PreviewState, TEXT("actor_id"), ActorId)
				&& !TryGetStringField(PreviewState, TEXT("actor"), ActorId)
				&& !TryGetStringField(PreviewState, TEXT("actor_name"), ActorId)
				&& !TryGetStringField(PreviewState, TEXT("actor_label"), ActorId))
			{
				OutError = TEXT("missing_level_preview_actor_id");
				return false;
			}
			FString ComponentId;
			if (!TryGetStringField(PreviewState, TEXT("component_id"), ComponentId)
				&& !TryGetStringField(PreviewState, TEXT("component"), ComponentId)
				&& !TryGetStringField(PreviewState, TEXT("component_name"), ComponentId))
			{
				OutError = TEXT("missing_level_preview_component_id");
				return false;
			}

			FString WorldError;
			UWorld* World = Server.GetEditorWorld(WorldError);
			if (!World)
			{
				OutError = WorldError.IsEmpty() ? TEXT("no_editor_world") : WorldError;
				return false;
			}
			AActor* Actor = Server.FindActorByNameOrLabel(World, ActorId);
			if (!Actor)
			{
				OutError = TEXT("level_preview_actor_not_found");
				return false;
			}
			UActorComponent* ActorComponent = UeAgentLevelOps::FindComponentByNameOrPath(Actor, ComponentId);
			if (!ActorComponent)
			{
				OutError = TEXT("level_preview_component_not_found");
				return false;
			}
			UNiagaraComponent* NiagaraComponent = Cast<UNiagaraComponent>(ActorComponent);
			if (!NiagaraComponent)
			{
				OutError = TEXT("level_preview_component_not_niagara");
				return false;
			}

			double AbsoluteAgeNumber = 0.0;
			const bool bHasAbsoluteAge =
				TryGetNumberField(PreviewState, TEXT("absolute_time"), AbsoluteAgeNumber)
				|| TryGetNumberField(PreviewState, TEXT("absolute_time_seconds"), AbsoluteAgeNumber)
				|| TryGetNumberField(PreviewState, TEXT("time_seconds"), AbsoluteAgeNumber)
				|| TryGetNumberField(PreviewState, TEXT("seconds"), AbsoluteAgeNumber)
				|| TryGetNumberField(PreviewState, TEXT("desired_age"), AbsoluteAgeNumber)
				|| TryGetNumberField(PreviewState, TEXT("age"), AbsoluteAgeNumber);
			if (bHasAbsoluteAge && (!FMath::IsFinite(AbsoluteAgeNumber) || AbsoluteAgeNumber < 0.0))
			{
				OutError = TEXT("invalid_level_preview_absolute_time");
				return false;
			}
			bool bSimulation = true;
			const bool bHasSimulation = TryGetBoolField(PreviewState, TEXT("simulation"), bSimulation);
			bool bPaused = true;
			const bool bHasPaused =
				TryGetBoolField(PreviewState, TEXT("pause_after_advance"), bPaused)
				|| TryGetBoolField(PreviewState, TEXT("pause"), bPaused)
				|| TryGetBoolField(PreviewState, TEXT("paused"), bPaused);
			if (!bHasAbsoluteAge && !bHasSimulation && !bHasPaused)
			{
				OutError = TEXT("level_preview_state_missing_action");
				return false;
			}
			if (bHasAbsoluteAge && bHasSimulation && !bSimulation)
			{
				OutError = TEXT("level_preview_state_conflicting_simulation");
				return false;
			}

			CaptureNiagaraPreviewRestoreState(NiagaraComponent);
			const float AgeBefore = GetNiagaraComponentActualAge(NiagaraComponent);
			if (bHasAbsoluteAge)
			{
				double TickDeltaNumber = 1.0 / 60.0;
				TryGetNumberField(PreviewState, TEXT("tick_delta_seconds"), TickDeltaNumber);
				TryGetNumberField(PreviewState, TEXT("preview_tick_delta_seconds"), TickDeltaNumber);
				const float TickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaNumber), 1.0f / 240.0f, 1.0f / 5.0f);
				const float AbsoluteAge = static_cast<float>(AbsoluteAgeNumber);
				bool bReset = true;
				TryGetBoolField(PreviewState, TEXT("reset"), bReset);
				TryGetBoolField(PreviewState, TEXT("reset_preview"), bReset);

				if (UNiagaraSystem* NiagaraSystem = NiagaraComponent->GetAsset())
				{
					if (NiagaraSystem->HasOutstandingCompilationRequests(true))
					{
						NiagaraSystem->WaitForCompilationComplete(true, false);
						NiagaraSystem->PollForCompilationComplete(true);
					}
					NiagaraSystem->CacheFromCompiledData();
				}
				NiagaraComponent->SetPaused(false);
				float ActualAge = 0.0f;
				if (!SeekNiagaraComponentToAbsoluteAge(NiagaraComponent, World, AbsoluteAge, TickDeltaSeconds, bReset, ActualAge))
				{
					FString RestoreError;
					RestoreNiagaraPreviewState(RestoreError);
					OutError = TEXT("level_preview_seek_incomplete");
					return false;
				}
				NiagaraComponent->SetPaused(bHasPaused ? bPaused : true);
				NiagaraComponent->MarkRenderStateDirty();

				OutData->SetStringField(TEXT("mode"), TEXT("seek_absolute_time"));
				OutData->SetNumberField(TEXT("absolute_time_seconds"), AbsoluteAge);
				OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
				OutData->SetBoolField(TEXT("reset"), bReset);
				OutData->SetBoolField(TEXT("pause_after_advance"), bHasPaused ? bPaused : true);
				OutData->SetNumberField(TEXT("actual_age_seconds"), ActualAge);
			}
			else
			{
				if (bHasSimulation)
				{
					if (bSimulation)
					{
						NiagaraComponent->Activate(false);
					}
					else
					{
						NiagaraComponent->DeactivateImmediate();
					}
				}
				if (bHasPaused)
				{
					NiagaraComponent->SetPaused(bPaused);
				}
				OutData->SetStringField(TEXT("mode"), TEXT("simulation_state"));
			}

			OutData->SetBoolField(TEXT("applied"), true);
			OutData->SetStringField(TEXT("actor_id"), ActorId);
			OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
			OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			OutData->SetStringField(TEXT("component_id"), ComponentId);
			OutData->SetStringField(TEXT("component_name"), NiagaraComponent->GetName());
			OutData->SetStringField(TEXT("component_class"), NiagaraComponent->GetClass()->GetPathName());
			OutData->SetNumberField(TEXT("age_before_seconds"), AgeBefore);
			if (!OutData->HasField(TEXT("actual_age_seconds")))
			{
				OutData->SetNumberField(TEXT("actual_age_seconds"), GetNiagaraComponentActualAge(NiagaraComponent));
			}
			OutData->SetBoolField(TEXT("active"), NiagaraComponent->IsActive());
			OutData->SetBoolField(TEXT("paused"), NiagaraComponent->IsPaused());
			return true;
		}



		virtual bool SnapshotState(const FResolvedViewport& Viewport, FViewportSnapshot& OutSnapshot, FString& OutError) const override
		{
			OutSnapshot = FViewportSnapshot();
			if (!ReadState(Viewport, OutSnapshot.State, OutError))
			{
				return false;
			}
			OutSnapshot.bValid = true;
			return true;
		}

		virtual bool RestoreState(const FResolvedViewport& Viewport, const FViewportSnapshot& Snapshot, FString& OutError) const override
		{
			if (!Snapshot.bValid)
			{
				OutError = TEXT("invalid_snapshot");
				return false;
			}
			if (!WriteCamera(Viewport, Snapshot.State.Camera, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Applied;
			if (!ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, Applied, OutError))
			{
				return false;
			}
			return RestoreNiagaraPreviewState(OutError);
		}

	private:
		struct FNiagaraPreviewRestoreState
		{
			TWeakObjectPtr<UNiagaraComponent> Component;
			ENiagaraAgeUpdateMode AgeUpdateMode = ENiagaraAgeUpdateMode::TickDeltaTime;
			float DesiredAge = 0.0f;
			float ActualAge = 0.0f;
			float SeekDelta = 1.0f / 60.0f;
			bool bLockDesiredAgeDeltaTimeToSeekDelta = false;
			bool bForceSolo = false;
			bool bPaused = false;
			bool bActive = false;
			bool bValid = false;
		};

		const FUeAgentHttpServer& Server;
		mutable FNiagaraPreviewRestoreState NiagaraPreviewRestoreState;

		static float GetNiagaraComponentActualAge(UNiagaraComponent* Component)
		{
			if (!Component)
			{
				return 0.0f;
			}
			const FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
			return Controller.IsValid() ? Controller->GetAge() : Component->GetDesiredAge();
		}

		static bool SeekNiagaraComponentToAbsoluteAge(
			UNiagaraComponent* Component,
			UWorld* World,
			const float AbsoluteAge,
			const float TickDeltaSeconds,
			const bool bReset,
			float& OutActualAge)
		{
			OutActualAge = GetNiagaraComponentActualAge(Component);
			if (!Component || !World)
			{
				return false;
			}
			Component->SetForceSolo(true);
			if (bReset)
			{
				Component->DeactivateImmediate();
				Component->SetDesiredAge(0.0f);
				Component->ReinitializeSystem();
			}
			Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
			Component->SetSeekDelta(TickDeltaSeconds);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->Activate(false);
			Component->InitializeSystem();
			Component->SetDesiredAge(AbsoluteAge);
			Component->SeekToDesiredAge(AbsoluteAge);

			const float Tolerance = FMath::Max(TickDeltaSeconds * 0.5f, 0.0001f);
			const int32 MaxPasses = FMath::Clamp(FMath::CeilToInt(AbsoluteAge / TickDeltaSeconds) + 4, 4, 2048);
			for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
			{
				Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
				World->SendAllEndOfFrameUpdates();
				OutActualAge = GetNiagaraComponentActualAge(Component);
				if (FMath::Abs(OutActualAge - AbsoluteAge) <= Tolerance)
				{
					return true;
				}
			}

			float RemainingAge = AbsoluteAge - OutActualAge;
			if (RemainingAge > Tolerance)
			{
				const int32 FixedStepCount = FMath::Clamp(FMath::FloorToInt(RemainingAge / TickDeltaSeconds + KINDA_SMALL_NUMBER), 0, 2048);
				if (FixedStepCount > 0)
				{
					Component->AdvanceSimulation(FixedStepCount, TickDeltaSeconds);
					World->SendAllEndOfFrameUpdates();
					OutActualAge = GetNiagaraComponentActualAge(Component);
					RemainingAge = AbsoluteAge - OutActualAge;
				}
				if (RemainingAge > Tolerance)
				{
					Component->AdvanceSimulation(1, RemainingAge);
					World->SendAllEndOfFrameUpdates();
					OutActualAge = GetNiagaraComponentActualAge(Component);
				}
			}
			return FMath::Abs(OutActualAge - AbsoluteAge) <= Tolerance;
		}

		void CaptureNiagaraPreviewRestoreState(UNiagaraComponent* Component) const
		{
			if (!Component || NiagaraPreviewRestoreState.bValid)
			{
				return;
			}
			NiagaraPreviewRestoreState.Component = Component;
			NiagaraPreviewRestoreState.AgeUpdateMode = Component->GetAgeUpdateMode();
			NiagaraPreviewRestoreState.DesiredAge = Component->GetDesiredAge();
			NiagaraPreviewRestoreState.ActualAge = GetNiagaraComponentActualAge(Component);
			NiagaraPreviewRestoreState.SeekDelta = Component->GetSeekDelta();
			NiagaraPreviewRestoreState.bLockDesiredAgeDeltaTimeToSeekDelta = Component->GetLockDesiredAgeDeltaTimeToSeekDelta();
			NiagaraPreviewRestoreState.bForceSolo = Component->GetForceSolo();
			NiagaraPreviewRestoreState.bPaused = Component->IsPaused();
			NiagaraPreviewRestoreState.bActive = Component->IsActive();
			NiagaraPreviewRestoreState.bValid = true;
		}

		bool RestoreNiagaraPreviewState(FString& OutError) const
		{
			if (!NiagaraPreviewRestoreState.bValid)
			{
				return true;
			}
			const FNiagaraPreviewRestoreState RestoreState = NiagaraPreviewRestoreState;
			NiagaraPreviewRestoreState = FNiagaraPreviewRestoreState();
			UNiagaraComponent* Component = RestoreState.Component.Get();
			if (!Component)
			{
				return true;
			}

			Component->SetPaused(false);
			if (RestoreState.bActive)
			{
				UWorld* World = Component->GetWorld();
				float RestoredActualAge = 0.0f;
				if (!SeekNiagaraComponentToAbsoluteAge(
					Component,
					World,
					FMath::Max(RestoreState.ActualAge, 0.0f),
					FMath::Clamp(RestoreState.SeekDelta, 1.0f / 240.0f, 1.0f / 5.0f),
					true,
					RestoredActualAge))
				{
					OutError = TEXT("level_preview_restore_seek_incomplete");
					return false;
				}
			}
			else
			{
				Component->DeactivateImmediate();
			}
			Component->SetAgeUpdateMode(RestoreState.AgeUpdateMode);
			Component->SetDesiredAge(RestoreState.DesiredAge);
			Component->SetSeekDelta(RestoreState.SeekDelta);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(RestoreState.bLockDesiredAgeDeltaTimeToSeekDelta);
			Component->SetForceSolo(RestoreState.bForceSolo);
			Component->SetPaused(RestoreState.bPaused);
			Component->MarkRenderStateDirty();
			return true;
		}

		static FLevelEditorViewportClient* AsLevelClient(const FResolvedViewport& Viewport)
		{
			return static_cast<FLevelEditorViewportClient*>(Viewport.ViewportClient);
		}

		static bool TryParseViewportIndex(const FString& Token, int32& OutIndex)
		{
			const FString Normalized = NormalizeToken(Token);
			if (Normalized.IsEmpty())
			{
				return false;
			}
			FString Tail = Normalized;
			if (Normalized.StartsWith(TEXT("index_")))
			{
				Tail = Normalized.RightChop(6);
			}
			else if (Normalized.StartsWith(TEXT("level:")))
			{
				Tail = Normalized.RightChop(6);
			}
			return LexTryParseString(OutIndex, *Tail) && OutIndex >= 0;
		}

		static int32 GetClientIndex(const FLevelEditorViewportClient* TargetClient)
		{
			if (!GEditor || !TargetClient)
			{
				return INDEX_NONE;
			}
			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client == TargetClient)
				{
					return Index;
				}
				++Index;
			}
			return INDEX_NONE;
		}

		static FLevelEditorViewportClient* GetClientByIndex(const int32 TargetIndex, FString* OutSelectionReason = nullptr)
		{
			if (!GEditor || TargetIndex < 0)
			{
				return nullptr;
			}
			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Index == TargetIndex)
				{
					if (OutSelectionReason)
					{
						*OutSelectionReason = FString::Printf(TEXT("requested_level_viewport_%d"), Index);
					}
					return Client && Client->Viewport ? Client : nullptr;
				}
				++Index;
			}
			return nullptr;
		}

		static FLevelEditorViewportClient* GetActiveLevelClient(FString* OutSelectionReason = nullptr)
		{
			if (GEditor)
			{
				if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
				{
					for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
					{
						if (Client && Client->Viewport == ActiveViewport)
						{
							if (OutSelectionReason)
							{
								*OutSelectionReason = TEXT("active_viewport");
							}
							return Client;
						}
					}
				}
			}
			return nullptr;
		}

		static FLevelEditorViewportClient* GetRequestedOrPreferredClient(const FViewportTarget& Target, FString* OutSelectionReason = nullptr)
		{
			int32 RequestedIndex = INDEX_NONE;
			if (TryParseViewportIndex(Target.ViewportId, RequestedIndex) || TryParseViewportIndex(Target.Viewport, RequestedIndex))
			{
				return GetClientByIndex(RequestedIndex, OutSelectionReason);
			}
			if (Target.Viewport == TEXT("active"))
			{
				return GetActiveLevelClient(OutSelectionReason);
			}
			return GetPreferredClient(OutSelectionReason);
		}

		static FLevelEditorViewportClient* GetPreferredClient(FString* OutSelectionReason = nullptr)
		{
			auto SetReason = [OutSelectionReason](const TCHAR* Reason)
			{
				if (OutSelectionReason)
				{
					*OutSelectionReason = Reason;
				}
			};

			if (GEditor)
			{
				if (FLevelEditorViewportClient* ActiveClient = GetActiveLevelClient(nullptr))
				{
					if (ActiveClient->ViewportType == LVT_Perspective)
					{
						SetReason(TEXT("active_perspective_viewport"));
						return ActiveClient;
					}
				}

				int32 Index = 0;
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->Viewport && Client->ViewportType == LVT_Perspective)
					{
						SetReason(*FString::Printf(TEXT("perspective_level_viewport_client_%d"), Index));
						return Client;
					}
					++Index;
				}

				if (FLevelEditorViewportClient* ActiveClient = GetActiveLevelClient(nullptr))
				{
					SetReason(TEXT("active_viewport"));
					return ActiveClient;
				}

				Index = 0;
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->Viewport)
					{
						SetReason(*FString::Printf(TEXT("level_viewport_client_%d"), Index));
						return Client;
					}
					++Index;
				}
			}

			if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
			{
				SetReason(TEXT("current_level_editing_viewport"));
				return GCurrentLevelEditingViewportClient;
			}

			SetReason(TEXT("none"));
			return nullptr;
		}
	};

	TSharedRef<IViewportAdapter> MakeLevelViewportAdapter(const FUeAgentHttpServer& Server)
	{
		return MakeShared<FLevelViewportAdapter>(Server);
	}
}
