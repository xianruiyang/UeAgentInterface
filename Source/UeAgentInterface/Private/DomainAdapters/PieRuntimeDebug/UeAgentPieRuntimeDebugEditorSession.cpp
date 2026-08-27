// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugEditorSession.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ScopeLock.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Slate/SceneViewport.h"
#include "UnrealEdGlobals.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ContextResource;
	using namespace EditorSessionOperation;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* DebugPolicyId =
			TEXT("policy:runtime-session:pie-debug/v1");
		const TCHAR* SubjectPrefix = TEXT("runtime-subject:pie:");

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(MoveTemp(Code), MoveTemp(Detail));
			return false;
		}

		void DestroyHiddenSlateWindow(TSharedPtr<SWindow> Window)
		{
			if (Window.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			}
		}

		bool TryReadRequest(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			int32& OutPieInstance,
			int32& OutLocalPlayerIndex,
			FExecutionMutationError& OutError)
		{
			if (Request.GetFamily()
					!= EUeAgentEditorSessionOperationFamily::RuntimePreview
				|| Request.GetPolicyId() != DebugPolicyId
				|| !Request.GetSubjectRef().StartsWith(SubjectPrefix))
			{
				return Fail(OutError, TEXT("pie_runtime_editor_session_request_invalid"));
			}

			const FString InstanceText = Request.GetSubjectRef().RightChop(
				FCString::Strlen(SubjectPrefix));
			if (InstanceText.IsEmpty()
				|| !LexTryParseString(OutPieInstance, *InstanceText)
				|| OutPieInstance < 0)
			{
				return Fail(OutError, TEXT("pie_runtime_instance_invalid"));
			}

			const FUeAgentCanonicalValue& Parameters = Request.GetParameters().Get();
			if (Parameters.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, TEXT("pie_runtime_editor_parameters_invalid"));
			}
			OutLocalPlayerIndex = 0;
			bool bHasLocalPlayerIndex = false;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Parameters.GetObjectItems())
			{
				if (Item.Key != TEXT("local_player_index")
					|| bHasLocalPlayerIndex
					|| Item.Value->GetKind() != EUeAgentValueKind::Integer
					|| !LexTryParseString(
						OutLocalPlayerIndex, *Item.Value->GetLexeme())
					|| OutLocalPlayerIndex != 0)
				{
					return Fail(
						OutError, TEXT("pie_runtime_editor_parameters_invalid"));
				}
				bHasLocalPlayerIndex = true;
			}
			return true;
		}

		bool IsActivationPendingError(
			const FString& Error,
			int32 PieInstance)
		{
			if (Error == TEXT("pie_runtime_instance_not_found"))
			{
				if (!GEngine)
				{
					return true;
				}
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE && Context.World())
					{
						return false;
					}
				}
				return true;
			}
			if (Error == TEXT("pie_runtime_game_instance_missing")
				|| Error == TEXT("pie_runtime_player_controller_missing"))
			{
				return true;
			}
			if (Error != TEXT("pie_runtime_requires_exactly_one_local_player"))
			{
				return false;
			}
			UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
			FWorldContext* Context = EditorEngine
				? EditorEngine->GetPIEWorldContext(PieInstance)
				: nullptr;
			UGameInstance* GameInstance = Context && Context->World()
				? Context->World()->GetGameInstance()
				: nullptr;
			return GameInstance && GameInstance->GetLocalPlayers().IsEmpty();
		}

		enum class EDebugEditorSessionState : uint8
		{
			Scheduled,
			Launching,
			Active,
			Stopping,
			ExitedSuccess,
			ExitedFailure,
		};

		enum class EHiddenPieSurfaceOwnership : uint8
		{
			None,
			Session,
			Editor,
			Closed,
		};

		class FPieRuntimeDebugEditorSession final
			: public IUeAgentEditorSessionNativeSession
			, public TSharedFromThis<
				FPieRuntimeDebugEditorSession,
				ESPMode::ThreadSafe>
		{
		public:
			FPieRuntimeDebugEditorSession(
				int32 InPieInstance,
				int32 InLocalPlayerIndex,
				TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe>
					InRegistry)
				: PieInstance(InPieInstance)
				, LocalPlayerIndex(InLocalPlayerIndex)
				, Registry(MoveTemp(InRegistry))
			{
			}

			~FPieRuntimeDebugEditorSession() override
			{
				FTSTicker::FDelegateHandle LocalLifecycleTicker;
				FTSTicker::FDelegateHandle LocalStopTicker;
				TSharedPtr<SWindow> LocalHiddenPieWindow;
				EHiddenPieSurfaceOwnership LocalWindowOwnership =
					EHiddenPieSurfaceOwnership::None;
				{
					FScopeLock Lock(&Mutex);
					LocalLifecycleTicker = LifecycleTicker;
					LocalStopTicker = StopTicker;
					LifecycleTicker.Reset();
					StopTicker.Reset();
					LocalHiddenPieWindow = MoveTemp(HiddenPieWindow);
					LocalWindowOwnership = HiddenPieWindowOwnership;
				}
				if (LocalLifecycleTicker.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(LocalLifecycleTicker);
				}
				if (LocalStopTicker.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(LocalStopTicker);
				}
				if (LocalHiddenPieWindow.IsValid()
					&& LocalWindowOwnership == EHiddenPieSurfaceOwnership::Session)
				{
					if (IsInGameThread())
					{
						DestroyHiddenSlateWindow(MoveTemp(LocalHiddenPieWindow));
					}
					else
					{
						AsyncTask(
							ENamedThreads::GameThread,
							[Window = MoveTemp(LocalHiddenPieWindow)]() mutable
							{
								DestroyHiddenSlateWindow(MoveTemp(Window));
							});
					}
				}
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FString& OutNativeResourceId,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bStarted)
				{
					return Fail(
						OutError, TEXT("pie_runtime_editor_session_already_started"));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(
						OutError,
						TEXT("pie_runtime_editor_session_cancelled_before_start"));
				}
				bStarted = true;
				CancellationToken = MakeUnique<FUeAgentCancellationToken>(Cancellation);
				NativeResourceId = TEXT("pie-runtime-editor-native:")
					+ OperationRequest.GetOperationId();
				OutNativeResourceId = NativeResourceId;
				const TSharedRef<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					Self = StaticCastSharedRef<FPieRuntimeDebugEditorSession>(AsShared());
				AsyncTask(ENamedThreads::GameThread, [Self]() { Self->LaunchOnGameThread(); });
				return true;
			}

			bool Observe(
				FUeAgentEditorSessionNativeObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				EDebugEditorSessionState ObservedState;
				FString ObservedErrorCode;
				FString ObservedSessionRef;
				{
					FScopeLock Lock(&Mutex);
					if (!bStarted)
					{
						return Fail(
							OutError, TEXT("pie_runtime_editor_session_not_started"));
					}
					ObservedState = State;
					ObservedErrorCode = ErrorCode;
					ObservedSessionRef = SessionRef;
				}

				FUeAgentPieRuntimeDebugCachedSnapshot CurrentSnapshot;
				if (ObservedState == EDebugEditorSessionState::Active)
				{
					FString SnapshotError;
					if (!Registry->TryGetCachedSnapshot(
							ObservedSessionRef, CurrentSnapshot, SnapshotError))
					{
						return Fail(OutError, MoveTemp(SnapshotError));
					}
				}

				ELongRunningWorkState WorkState = ELongRunningWorkState::Running;
				FString Phase = TEXT("scheduled");
				if (ObservedState == EDebugEditorSessionState::Launching)
				{
					Phase = TEXT("launching");
				}
				else if (ObservedState == EDebugEditorSessionState::Active)
				{
					Phase = TEXT("active");
				}
				else if (ObservedState == EDebugEditorSessionState::Stopping)
				{
					Phase = TEXT("stopping");
				}
				else if (ObservedState == EDebugEditorSessionState::ExitedSuccess)
				{
					WorkState = ELongRunningWorkState::ExitedSuccess;
					Phase = TEXT("exited");
				}
				else if (ObservedState == EDebugEditorSessionState::ExitedFailure)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("failed");
				}
				TSharedPtr<FJsonObject> Details;
				if (ObservedState == EDebugEditorSessionState::Active)
				{
					Details = MakeShared<FJsonObject>();
					Details->SetNumberField(
						TEXT("pause_epoch"), CurrentSnapshot.PauseEpoch);
					Details->SetNumberField(
						TEXT("workspace_revision"), CurrentSnapshot.WorkspaceRevision);
					Details->SetStringField(
						TEXT("workspace_path"), CurrentSnapshot.WorkspacePath);
					Details->SetNumberField(
						TEXT("pie_instance"), CurrentSnapshot.PieInstance);
					Details->SetNumberField(
						TEXT("local_player_index"), CurrentSnapshot.LocalPlayerIndex);
				}
				OutObservation = FUeAgentEditorSessionNativeObservation(
					WorkState,
					MoveTemp(Phase),
					MoveTemp(ObservedErrorCode),
					MoveTemp(ObservedSessionRef),
					MoveTemp(Details));
				return true;
			}

			bool RequestStop(
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				{
					FScopeLock Lock(&Mutex);
					if (!bStarted)
					{
						return Fail(
							OutError, TEXT("pie_runtime_editor_session_not_started"));
					}
					if (State == EDebugEditorSessionState::ExitedSuccess
						|| State == EDebugEditorSessionState::ExitedFailure
						|| bStopRequested)
					{
						return true;
					}
					bStopRequested = true;
				}
				const TSharedRef<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					Self = StaticCastSharedRef<FPieRuntimeDebugEditorSession>(AsShared());
				AsyncTask(
					ENamedThreads::GameThread,
					[Self]() { Self->BeginStopOnGameThread(true, FString()); });
				return true;
			}

			bool Join(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				return State == EDebugEditorSessionState::ExitedSuccess
					|| State == EDebugEditorSessionState::ExitedFailure
					? true
					: Fail(
						OutError, TEXT("pie_runtime_editor_session_join_before_exit"));
			}

			bool Release(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				bReleased = true;
				return true;
			}

		private:
			bool TryCreateHiddenPieSurfaceOnGameThread(
				FRequestPlaySessionParams& OutParams,
				FString& OutError)
			{
				check(IsInGameThread());
				if (!FSlateApplication::IsInitialized())
				{
					OutError = TEXT("pie_runtime_editor_slate_unavailable");
					return false;
				}
				ULevelEditorPlaySettings* PlaySettings = DuplicateObject<
					ULevelEditorPlaySettings>(
						GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
				if (!PlaySettings)
				{
					OutError = TEXT("pie_runtime_editor_play_settings_create_failed");
					return false;
				}
				PlaySettings->GameGetsMouseControl = false;
				PlaySettings->UseMouseForTouch = false;
				PlaySettings->ShowMouseControlLabel = false;
				PlaySettings->ViewportGetsHMDControl = false;
				PlaySettings->ShouldMinimizeEditorOnVRPIE = false;
				PlaySettings->bShouldMinimizeEditorOnNonVRPIE = false;
				PlaySettings->EnableGameSound = false;
				PlaySettings->SoloAudioInFirstPIEClient = false;
				PlaySettings->EnablePIEEnterAndExitSounds = false;
				PlaySettings->PIEAlwaysOnTop = false;
				PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
				PlaySettings->SetRunUnderOneProcess(true);
				PlaySettings->SetPlayNumberOfClients(1);
				PlaySettings->bLaunchSeparateServer = false;

				const TSharedRef<SWindow> Window = SNew(SWindow)
					.Title(FText::FromString(TEXT("UAI Background PIE")))
					.Tag(TEXT("UAIBackgroundPIEWindow"))
					.ClientSize(FVector2D(1280.0, 720.0))
					.AutoCenter(EAutoCenter::None)
					.ScreenPosition(FVector2D::ZeroVector)
					.SizingRule(ESizingRule::FixedSize)
					.FocusWhenFirstShown(false)
					.ActivationPolicy(EWindowActivationPolicy::Never)
					.IsTopmostWindow(false)
					.HasCloseButton(false)
					.SupportsMaximize(false)
					.SupportsMinimize(false)
					.CreateTitleBar(false)
					.SaneWindowPlacement(false);
				Window->SetAllowFastUpdate(true);
				{
					FScopeLock Lock(&Mutex);
					HiddenPieWindow = Window;
					HiddenPieWindowOwnership = EHiddenPieSurfaceOwnership::Session;
				}
				const TWeakPtr<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				Window->GetOnWindowClosedEvent().AddLambda(
					[WeakSelf](const TSharedRef<SWindow>& ClosedWindow)
					{
						const TSharedPtr<
							FPieRuntimeDebugEditorSession,
							ESPMode::ThreadSafe> Self = WeakSelf.Pin();
						if (Self.IsValid())
						{
							Self->MarkHiddenPieSurfaceClosed(ClosedWindow);
						}
					});
				FSlateApplication::Get().AddWindow(Window, false);
				OutParams.CustomPIEWindow = Window;
				OutParams.EditorPlaySettings = PlaySettings;
				return true;
			}

			void ReleaseHiddenPieSurfaceOnGameThread()
			{
				check(IsInGameThread());
				TSharedPtr<SWindow> Window;
				EHiddenPieSurfaceOwnership WindowOwnership =
					EHiddenPieSurfaceOwnership::None;
				{
					FScopeLock Lock(&Mutex);
					Window = MoveTemp(HiddenPieWindow);
					WindowOwnership = HiddenPieWindowOwnership;
					HiddenPieWindowOwnership = EHiddenPieSurfaceOwnership::None;
				}
				if (WindowOwnership == EHiddenPieSurfaceOwnership::Session)
				{
					DestroyHiddenSlateWindow(MoveTemp(Window));
				}
			}

			void MarkHiddenPieSurfaceClosed(const TSharedRef<SWindow>& ClosedWindow)
			{
				FScopeLock Lock(&Mutex);
				if (HiddenPieWindow.Get() == &ClosedWindow.Get())
				{
					HiddenPieWindowOwnership = EHiddenPieSurfaceOwnership::Closed;
				}
			}

			void RefreshHiddenPieSurfaceOwnershipOnGameThread()
			{
				check(IsInGameThread());
				TSharedPtr<SWindow> Window;
				{
					FScopeLock Lock(&Mutex);
					if (HiddenPieWindowOwnership
						!= EHiddenPieSurfaceOwnership::Session)
					{
						return;
					}
					Window = HiddenPieWindow;
				}
				UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
				UGameViewportClient* ViewportClient = PieWorld
					? PieWorld->GetGameViewport()
					: nullptr;
				FSceneViewport* SceneViewport = ViewportClient
					? ViewportClient->GetGameViewport()
					: nullptr;
				const TSharedPtr<SViewport> ViewportWidget = SceneViewport
					? SceneViewport->GetViewportWidget().Pin()
					: nullptr;
				const TSharedPtr<SWindow> EditorWindow =
					ViewportWidget.IsValid() && FSlateApplication::IsInitialized()
					? FSlateApplication::Get().FindWidgetWindow(
						ViewportWidget.ToSharedRef())
					: nullptr;
				if (Window.IsValid() && EditorWindow == Window)
				{
					FScopeLock Lock(&Mutex);
					if (HiddenPieWindow == Window)
					{
						HiddenPieWindowOwnership = EHiddenPieSurfaceOwnership::Editor;
					}
				}
			}

			void LaunchOnGameThread()
			{
				check(IsInGameThread());
				{
					FScopeLock Lock(&Mutex);
					if (bStopRequested)
					{
						// The already queued stop task owns cleanup.
						return;
					}
					if (!GUnrealEd || !GEditor || GEditor->PlayWorld
						|| GUnrealEd->GetPlaySessionRequest().IsSet())
					{
						State = EDebugEditorSessionState::ExitedFailure;
						ErrorCode = TEXT("pie_runtime_editor_launch_unavailable");
						return;
					}
					State = EDebugEditorSessionState::Launching;
				}

				FRequestPlaySessionParams Params;
				Params.SessionDestination = EPlaySessionDestinationType::InProcess;
				Params.WorldType = EPlaySessionWorldType::PlayInEditor;
				FString SurfaceError;
				if (!TryCreateHiddenPieSurfaceOnGameThread(Params, SurfaceError))
				{
					FScopeLock Lock(&Mutex);
					State = EDebugEditorSessionState::ExitedFailure;
					ErrorCode = MoveTemp(SurfaceError);
					return;
				}
				GUnrealEd->RequestPlaySession(Params);

				const TWeakPtr<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakSelf](float)
							{
								const TSharedPtr<
									FPieRuntimeDebugEditorSession,
									ESPMode::ThreadSafe> Self = WeakSelf.Pin();
								return Self.IsValid()
									? Self->TickLifecycleOnGameThread()
									: false;
							}),
						0.0f);
				if (!Handle.IsValid())
				{
					{
						FScopeLock Lock(&Mutex);
						State = EDebugEditorSessionState::ExitedFailure;
						ErrorCode =
							TEXT("pie_runtime_editor_launch_poll_schedule_failed");
					}
					if (GEditor && GEditor->PlayWorld)
					{
						{
							FScopeLock Lock(&Mutex);
							if (HiddenPieWindowOwnership
								== EHiddenPieSurfaceOwnership::Session)
							{
								HiddenPieWindowOwnership =
									EHiddenPieSurfaceOwnership::Editor;
							}
						}
						GUnrealEd->RequestEndPlayMap();
					}
					else if (GUnrealEd->GetPlaySessionRequest().IsSet())
					{
						GUnrealEd->CancelRequestPlaySession();
						ReleaseHiddenPieSurfaceOnGameThread();
					}
					else
					{
						ReleaseHiddenPieSurfaceOnGameThread();
					}
					return;
				}
				{
					FScopeLock Lock(&Mutex);
					LifecycleTicker = Handle;
				}
			}

			bool TickLifecycleOnGameThread()
			{
				check(IsInGameThread());
				RefreshHiddenPieSurfaceOwnershipOnGameThread();
				bool bLocalStopRequested = false;
				bool bCancelled = false;
				EDebugEditorSessionState LocalState;
				{
					FScopeLock Lock(&Mutex);
					bLocalStopRequested = bStopRequested;
					bCancelled = CancellationToken.IsValid()
						&& CancellationToken->IsCancelled();
					LocalState = State;
				}
				if (bLocalStopRequested || bCancelled)
				{
					BeginStopOnGameThread(true, FString());
					return false;
				}

				if (LocalState == EDebugEditorSessionState::Active)
				{
					if (!ActiveWorld.IsValid())
					{
						BeginStopOnGameThread(
							false, TEXT("pie_runtime_editor_world_lost"));
						return false;
					}
					return true;
				}

				FUeAgentPieRuntimeDebugSnapshot ActivatedSnapshot;
				FString ActivationError;
				if (Registry->TryActivate(
						PieInstance,
						LocalPlayerIndex,
						ActivatedSnapshot,
						ActivationError))
				{
					FScopeLock Lock(&Mutex);
					ActiveWorld = ActivatedSnapshot.World;
					SessionRef = MoveTemp(ActivatedSnapshot.SessionRef);
					if (HiddenPieWindowOwnership
						== EHiddenPieSurfaceOwnership::Session)
					{
						HiddenPieWindowOwnership = EHiddenPieSurfaceOwnership::Editor;
					}
					State = EDebugEditorSessionState::Active;
					return true;
				}
				if (IsActivationPendingError(ActivationError, PieInstance))
				{
					return true;
				}
				BeginStopOnGameThread(false, MoveTemp(ActivationError));
				return false;
			}

			void BeginStopOnGameThread(bool bRequestedSuccess, FString FailureCode)
			{
				check(IsInGameThread());
				RefreshHiddenPieSurfaceOwnershipOnGameThread();
				FString ActiveSessionRef;
				{
					FScopeLock Lock(&Mutex);
					if (State == EDebugEditorSessionState::ExitedSuccess
						|| State == EDebugEditorSessionState::ExitedFailure
						|| State == EDebugEditorSessionState::Stopping)
					{
						return;
					}
					State = EDebugEditorSessionState::Stopping;
					bTerminalSuccessRequested = bRequestedSuccess;
					PendingFailureCode = MoveTemp(FailureCode);
					ActiveSessionRef = SessionRef;
					LifecycleTicker.Reset();
				}

				if (!ActiveSessionRef.IsEmpty())
				{
					FString RegistryError;
					if (!Registry->TryEndSession(ActiveSessionRef, RegistryError))
					{
						FScopeLock Lock(&Mutex);
						bTerminalSuccessRequested = false;
						bRegistryCleanupPending = true;
						if (PendingFailureCode.IsEmpty())
						{
							PendingFailureCode = RegistryError;
						}
					}
				}

				if (!GUnrealEd)
				{
					CompleteStopOnGameThread(
						false, TEXT("pie_runtime_editor_stop_unavailable"));
					return;
				}
				if (GEditor && GEditor->PlayWorld)
				{
					GUnrealEd->RequestEndPlayMap();
				}
				else if (GUnrealEd->GetPlaySessionRequest().IsSet())
				{
					GUnrealEd->CancelRequestPlaySession();
				}

				const TWeakPtr<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakSelf](float)
							{
								const TSharedPtr<
									FPieRuntimeDebugEditorSession,
									ESPMode::ThreadSafe> Self = WeakSelf.Pin();
								return Self.IsValid()
									? Self->TickStopOnGameThread()
									: false;
							}),
						0.05f);
				FScopeLock Lock(&Mutex);
				if (!Handle.IsValid())
				{
					State = EDebugEditorSessionState::ExitedFailure;
					ErrorCode = TEXT("pie_runtime_editor_stop_poll_schedule_failed");
				}
				else
				{
					StopTicker = Handle;
				}
			}

			bool TickStopOnGameThread()
			{
				check(IsInGameThread());
				if ((GEditor && GEditor->PlayWorld)
					|| (GUnrealEd && GUnrealEd->GetPlaySessionRequest().IsSet()))
				{
					return true;
				}
				bool bRetryRegistryCleanup = false;
				FString ActiveSessionRef;
				{
					FScopeLock Lock(&Mutex);
					bRetryRegistryCleanup = bRegistryCleanupPending;
					ActiveSessionRef = SessionRef;
				}
				if (bRetryRegistryCleanup && !ActiveSessionRef.IsEmpty())
				{
					FString RegistryError;
					if (!Registry->TryEndSession(ActiveSessionRef, RegistryError))
					{
						CompleteStopOnGameThread(false, MoveTemp(RegistryError));
						return false;
					}
					FScopeLock Lock(&Mutex);
					bRegistryCleanupPending = false;
				}
				CompleteStopOnGameThread(true, FString());
				return false;
			}

			void CompleteStopOnGameThread(bool bCleanupSucceeded, FString CleanupError)
			{
				ReleaseHiddenPieSurfaceOnGameThread();
				FScopeLock Lock(&Mutex);
				StopTicker.Reset();
				const bool bSuccess = bCleanupSucceeded
					&& bTerminalSuccessRequested
					&& PendingFailureCode.IsEmpty();
				State = bSuccess
					? EDebugEditorSessionState::ExitedSuccess
					: EDebugEditorSessionState::ExitedFailure;
				ErrorCode = bSuccess
					? FString()
					: (!PendingFailureCode.IsEmpty()
						? PendingFailureCode
						: MoveTemp(CleanupError));
				if (!bSuccess && ErrorCode.IsEmpty())
				{
					ErrorCode = TEXT("pie_runtime_editor_stop_failed");
				}
			}

			const int32 PieInstance;
			const int32 LocalPlayerIndex;
			TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe> Registry;
			FCriticalSection Mutex;
			TUniquePtr<FUeAgentCancellationToken> CancellationToken;
			TWeakObjectPtr<UWorld> ActiveWorld;
			FString NativeResourceId;
			FString SessionRef;
			FString ErrorCode;
			FString PendingFailureCode;
			FTSTicker::FDelegateHandle LifecycleTicker;
			FTSTicker::FDelegateHandle StopTicker;
			TSharedPtr<SWindow> HiddenPieWindow;
			EHiddenPieSurfaceOwnership HiddenPieWindowOwnership =
				EHiddenPieSurfaceOwnership::None;
			EDebugEditorSessionState State = EDebugEditorSessionState::Scheduled;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bReleased = false;
			bool bTerminalSuccessRequested = false;
			bool bRegistryCleanupPending = false;
		};
	}

	bool TryCreatePieRuntimeDebugEditorSession(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe> Registry,
		TSharedPtr<IUeAgentEditorSessionNativeSession, ESPMode::ThreadSafe>&
			OutSession,
		FExecutionMutationError& OutError)
	{
		OutSession.Reset();
		int32 PieInstance = INDEX_NONE;
		int32 LocalPlayerIndex = INDEX_NONE;
		if (!TryReadRequest(
				Request, PieInstance, LocalPlayerIndex, OutError))
		{
			return false;
		}
		OutSession = MakeShared<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>(
			PieInstance, LocalPlayerIndex, MoveTemp(Registry));
		return true;
	}
}
