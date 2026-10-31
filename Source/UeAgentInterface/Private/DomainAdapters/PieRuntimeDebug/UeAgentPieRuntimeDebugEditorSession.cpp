// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugEditorSession.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationContract.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentPieLaunchPolicy.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"

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
#include "RHIGlobals.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Slate/SceneViewport.h"
#include "UnrealEdGlobals.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWindow.h"
#include "Widgets/SViewport.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ContextResource;
	using namespace EditorSessionOperation;
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using namespace PieDiagnostics;

	namespace
	{
		const TCHAR* DebugPolicyId =
			TEXT("policy:runtime-session:pie-debug/v2");
		const TCHAR* SubjectPrefix = TEXT("runtime-subject:pie:");
		const FIntPoint DefaultPieViewportSize(1280, 720);

		struct FPieWindowPreferences
		{
			FIntPoint Size;
			FIntPoint Position;
			FIntPoint LastSize;
			TArray<FIntPoint> InstancePositions;
			bool bCaptured = false;

			void Capture()
			{
				const ULevelEditorPlaySettings* Settings = GetDefault<ULevelEditorPlaySettings>();
				Size = FIntPoint(Settings->NewWindowWidth, Settings->NewWindowHeight);
				Position = Settings->NewWindowPosition;
				LastSize = Settings->LastSize;
				InstancePositions = Settings->MultipleInstancePositions;
				bCaptured = true;
			}

			bool Restore(UE::UAI::Contract::FDiagnostic& Error)
			{
				if (!bCaptured) return true;
				ULevelEditorPlaySettings* Settings = GetMutableDefault<ULevelEditorPlaySettings>();
				Settings->NewWindowWidth = Size.X;
				Settings->NewWindowHeight = Size.Y;
				Settings->NewWindowPosition = Position;
				Settings->LastSize = LastSize;
				Settings->MultipleInstancePositions = InstancePositions;
				Settings->PostEditChange();
				Settings->SaveConfig();
				TStrongObjectPtr<ULevelEditorPlaySettings> Readback(
					DuplicateObject<ULevelEditorPlaySettings>(Settings, GetTransientPackage()));
				const FString Filename = EditorProject::SettingsConfigFilename(*Settings);
				if (!EditorProject::ReadSettingsConfig(*Readback, *Settings, Filename, Error)) return false;
				if (Readback->NewWindowWidth != Size.X || Readback->NewWindowHeight != Size.Y
					|| Readback->NewWindowPosition != Position || Readback->MultipleInstancePositions != InstancePositions)
				{
					const auto PositionsFact = [](const TArray<FIntPoint>& Positions) {
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const FIntPoint& Point : Positions)
							Values.Add(Facts({{TEXT("x"), Number(Point.X)}, {TEXT("y"), Number(Point.Y)}}));
						return MakeShared<FJsonValueArray>(MoveTemp(Values));
					};
					Error = Native(TEXT("pie_runtime_window_preferences_restore_failed"),
						TEXT("The saved Play window preferences differ from their values before native PIE shutdown."),
						TEXT("pie_session_cleanup"), TEXT("window_preferences"),
						Facts({{TEXT("width"), Number(Readback->NewWindowWidth)}, {TEXT("height"), Number(Readback->NewWindowHeight)},
							{TEXT("position_x"), Number(Readback->NewWindowPosition.X)}, {TEXT("position_y"), Number(Readback->NewWindowPosition.Y)},
							{TEXT("instance_positions"), PositionsFact(Readback->MultipleInstancePositions)}}),
						Facts({{TEXT("width"), Number(Size.X)}, {TEXT("height"), Number(Size.Y)},
							{TEXT("position_x"), Number(Position.X)}, {TEXT("position_y"), Number(Position.Y)},
							{TEXT("instance_positions"), PositionsFact(InstancePositions)}}));
					Error.Source.SourceFile = Filename;
					Error.TargetPath = Settings->GetPathName();
					return false;
				}
				bCaptured = false;
				return true;
			}
		};

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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
			FIntPoint& OutViewportSize,
			FExecutionMutationError& OutError)
		{
			if (Request.GetFamily()
					!= EUeAgentEditorSessionOperationFamily::RuntimePreview
				|| Request.GetPolicyId() != DebugPolicyId
				|| !Request.GetSubjectRef().StartsWith(SubjectPrefix))
			{
				if (Request.GetFamily() != EUeAgentEditorSessionOperationFamily::RuntimePreview)
				{
					const auto* Family = FindEditorSessionOperationFamilyContract(Request.GetFamily());
					const TSharedPtr<FJsonValue> Actual = Family
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Family->GetFamilyId()))
						: Facts({ { TEXT("enum_value"), Number(static_cast<int32>(Request.GetFamily())) } });
					return Fail(OutError, Input(TEXT("pie_runtime_editor_session_request_invalid"), TEXT("PIE debugging requires the runtime_preview editor session family."),
						TEXT("execution"), TEXT("/family_id"), Actual, MakeShared<FJsonValueString>(TEXT("runtime_preview"))));
				}
				if (Request.GetPolicyId() != DebugPolicyId)
					return Fail(OutError, Input(TEXT("pie_runtime_editor_session_request_invalid"), TEXT("The editor session policy does not match the PIE debug policy."),
						TEXT("execution"), TEXT("/policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(DebugPolicyId)));
				return Fail(OutError, Input(TEXT("pie_runtime_editor_session_request_invalid"), TEXT("The PIE subject reference has an unsupported prefix."),
					TEXT("execution"), TEXT("/subject_ref"), MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("runtime-subject:pie:<nonnegative instance>"))));
			}

			const FString InstanceText = Request.GetSubjectRef().RightChop(
				FCString::Strlen(SubjectPrefix));
			if (InstanceText.IsEmpty()
				|| !LexTryParseString(OutPieInstance, *InstanceText)
				|| OutPieInstance < 0)
			{
				return Fail(OutError, Input(TEXT("pie_runtime_instance_invalid"), TEXT("The PIE subject reference must contain a nonnegative integer instance."),
					TEXT("execution"), TEXT("/subject_ref"), MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("runtime-subject:pie:<nonnegative int32>"))));
			}

			const FUeAgentCanonicalValue& Parameters = Request.GetParameters().Get();
			if (Parameters.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, Input(TEXT("pie_runtime_editor_parameters_invalid"), TEXT("PIE editor session parameters must be an object."),
					TEXT("execution"), TEXT("/parameters"), CanonicalValueFact(Parameters), MakeShared<FJsonValueString>(TEXT("object"))));
			}
			OutLocalPlayerIndex = 0;
			OutViewportSize = DefaultPieViewportSize;
			TSet<FString> SeenParameters;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Parameters.GetObjectItems())
			{
				const FString Token = Item.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				const FString Pointer = TEXT("/parameters/") + Token;
				const bool bPlayerIndex = Item.Key == TEXT("local_player_index");
				const bool bWidth = Item.Key == TEXT("viewport_width");
				const bool bHeight = Item.Key == TEXT("viewport_height");
				if (!bPlayerIndex && !bWidth && !bHeight)
					return Fail(OutError, Input(TEXT("pie_runtime_editor_parameters_invalid"), TEXT("The PIE editor session does not support this parameter."),
						TEXT("execution"), Pointer, MakeShared<FJsonValueString>(Item.Key), MakeShared<FJsonValueString>(TEXT("local_player_index, viewport_width, viewport_height"))));
				if (SeenParameters.Contains(Item.Key))
					return Fail(OutError, Input(TEXT("pie_runtime_editor_parameters_invalid"), TEXT("The PIE editor session parameter is specified more than once."),
						TEXT("execution"), Pointer, Number(2), Number(1)));
				SeenParameters.Add(Item.Key);
				int32 Value = 0;
				if (Item.Value->GetKind() != EUeAgentValueKind::Integer
					|| !LexTryParseString(Value, *Item.Value->GetLexeme())
					|| (bPlayerIndex ? Value != 0 : Value <= 0))
					return Fail(OutError, Input(TEXT("pie_runtime_editor_parameters_invalid"),
						bPlayerIndex ? TEXT("The PIE debug session requires local_player_index to be the integer zero.")
							: TEXT("A PIE viewport dimension must be a positive int32 pixel count."),
						TEXT("execution"), Pointer, CanonicalValueFact(Item.Value.Get()),
						bPlayerIndex ? Number(0) : Facts({{TEXT("minimum"), Number(1)}, {TEXT("maximum"), Number(MAX_int32)}})));
				if (bPlayerIndex) OutLocalPlayerIndex = Value;
				else if (bWidth) OutViewportSize.X = Value;
				else OutViewportSize.Y = Value;
			}
			return true;
		}

		bool IsActivationPendingError(
			const UE::UAI::Contract::FDiagnostic& Error,
			int32 PieInstance)
		{
			if (Error.Code == TEXT("pie_runtime_instance_not_found"))
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
			if (Error.Code == TEXT("pie_runtime_game_instance_missing")
				|| Error.Code == TEXT("pie_runtime_player_controller_missing"))
			{
				return true;
			}
			if (Error.Code != TEXT("pie_runtime_requires_exactly_one_local_player"))
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

		const TCHAR* StateName(EDebugEditorSessionState State)
		{
			switch (State)
			{
			case EDebugEditorSessionState::Scheduled: return TEXT("scheduled");
			case EDebugEditorSessionState::Launching: return TEXT("launching");
			case EDebugEditorSessionState::Active: return TEXT("active");
			case EDebugEditorSessionState::Stopping: return TEXT("stopping");
			case EDebugEditorSessionState::ExitedSuccess: return TEXT("exited_success");
			case EDebugEditorSessionState::ExitedFailure: return TEXT("exited_failure");
			}
			return TEXT("unknown");
		}

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
				FIntPoint InViewportSize,
				TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe>
					InRegistry)
				: PieInstance(InPieInstance)
				, LocalPlayerIndex(InLocalPlayerIndex)
				, ViewportSize(InViewportSize)
				, Registry(MoveTemp(InRegistry))
			{
			}

			~FPieRuntimeDebugEditorSession() override
			{
				const FDelegateHandle LocalPreEndHandle = PreEndHandle;
				const FDelegateHandle LocalShutdownHandle = ShutdownHandle;
				auto RemovePreferenceDelegates = [LocalPreEndHandle, LocalShutdownHandle]() {
					FEditorDelegates::PrePIEEnded.Remove(LocalPreEndHandle);
					FEditorDelegates::ShutdownPIE.Remove(LocalShutdownHandle);
				};
				if (IsInGameThread()) RemovePreferenceDelegates();
				else AsyncTask(ENamedThreads::GameThread, MoveTemp(RemovePreferenceDelegates));
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
					return Fail(OutError, Native(TEXT("pie_runtime_editor_session_already_started"), TEXT("The PIE editor session has already started."), TEXT("execution"), TEXT("started"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(OutError, Native(TEXT("pie_runtime_editor_session_cancelled_before_start"), TEXT("The PIE editor session was cancelled before it could start."), TEXT("execution"), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
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
				UE::UAI::Contract::FDiagnostic ObservedDiagnostic;
				FString ObservedSessionRef;
				{
					FScopeLock Lock(&Mutex);
					if (!bStarted)
					{
						return Fail(OutError, Native(TEXT("pie_runtime_editor_session_not_started"), TEXT("The PIE editor session has not started."), TEXT("execution"), TEXT("started"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					}
					ObservedState = State;
					ObservedDiagnostic = StateDiagnostic;
					ObservedSessionRef = SessionRef;
				}

				FUeAgentPieRuntimeDebugCachedSnapshot CurrentSnapshot;
				if (ObservedState == EDebugEditorSessionState::Active)
				{
					UE::UAI::Contract::FDiagnostic SnapshotError;
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
					MoveTemp(ObservedDiagnostic),
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
						return Fail(OutError, Native(TEXT("pie_runtime_editor_session_not_started"), TEXT("The PIE editor session has not started."), TEXT("execution"), TEXT("started"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
					[Self]() { Self->BeginStopOnGameThread(true, UE::UAI::Contract::FDiagnostic()); });
				return true;
			}

			bool Join(FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				return State == EDebugEditorSessionState::ExitedSuccess
					|| State == EDebugEditorSessionState::ExitedFailure
					? true
					: Fail(OutError, Native(TEXT("pie_runtime_editor_session_join_before_exit"), TEXT("The PIE editor session must reach a terminal state before joining."),
						TEXT("execution"), TEXT("state"), MakeShared<FJsonValueString>(StateName(State)), MakeShared<FJsonValueString>(TEXT("exited_success or exited_failure"))));
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
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				check(IsInGameThread());
				if (!FSlateApplication::IsInitialized())
				{
					OutError = Native(TEXT("pie_runtime_editor_slate_unavailable"), TEXT("Slate is unavailable for creating the PIE window."), TEXT("pie_session"), TEXT("slate_initialized"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				const uint32 MaxDimension = GetMax2DTextureDimension();
				if (static_cast<uint32>(ViewportSize.X) > MaxDimension || static_cast<uint32>(ViewportSize.Y) > MaxDimension)
				{
					const bool bWidth = static_cast<uint32>(ViewportSize.X) > MaxDimension;
					OutError = Input(TEXT("pie_runtime_viewport_dimension_unsupported"),
						TEXT("The requested PIE viewport dimension exceeds the active renderer's maximum 2D texture dimension."),
						TEXT("pie_session"), bWidth ? TEXT("/parameters/viewport_width") : TEXT("/parameters/viewport_height"),
						Number(bWidth ? ViewportSize.X : ViewportSize.Y),
						Facts({{TEXT("minimum"), Number(1)}, {TEXT("maximum"), Number(MaxDimension)}}));
					return false;
				}
				ULevelEditorPlaySettings* PlaySettings = DuplicateObject<
					ULevelEditorPlaySettings>(
						GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
				if (!PlaySettings)
				{
					OutError = Native(TEXT("pie_runtime_editor_play_settings_create_failed"), TEXT("The PIE session could not create its transient play settings."), TEXT("pie_session"), TEXT("play_settings_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					.ClientSize(FVector2D(ViewportSize))
					.AdjustInitialSizeAndPositionForDPIScale(false)
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
						const bool bEditorAvailable = GUnrealEd && GEditor;
						const TCHAR* Field = !bEditorAvailable ? TEXT("editor_available")
							: GEditor->PlayWorld ? TEXT("play_world_active") : TEXT("play_session_pending");
						StateDiagnostic = Native(TEXT("pie_runtime_editor_launch_unavailable"), TEXT("PIE cannot launch without an available editor and no active or pending play session."),
							TEXT("pie_session"), Field, MakeShared<FJsonValueBoolean>(bEditorAvailable), MakeShared<FJsonValueBoolean>(!bEditorAvailable));
						return;
					}
					State = EDebugEditorSessionState::Launching;
				}

				FRequestPlaySessionParams Params;
				Params.SessionDestination = EPlaySessionDestinationType::InProcess;
				Params.WorldType = EPlaySessionWorldType::PlayInEditor;
				UE::UAI::Contract::FDiagnostic SurfaceError;
				if (!TryCreateHiddenPieSurfaceOnGameThread(Params, SurfaceError))
				{
					FScopeLock Lock(&Mutex);
					State = EDebugEditorSessionState::ExitedFailure;
					StateDiagnostic = MoveTemp(SurfaceError);
					return;
				}
				const TWeakPtr<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>
					WeakSelf = AsShared();
				// Capture immediately before native shutdown, preserving any deliberate
				// preference edits made while this session was running. UE saves its
				// hidden window layout before ShutdownPIE; restore only those fields.
				const auto PreferenceOwner = AsShared();
				PreEndHandle = FEditorDelegates::PrePIEEnded.AddLambda([PreferenceOwner](bool) {
					PreferenceOwner->WindowPreferences.Capture();
				});
				ShutdownHandle = FEditorDelegates::ShutdownPIE.AddLambda([PreferenceOwner](bool) {
					const auto Self = PreferenceOwner;
					{
						UE::UAI::Contract::FDiagnostic Error;
						if (!Self->WindowPreferences.Restore(Error))
						{
							FScopeLock Lock(&Self->Mutex);
							Self->PendingDiagnostic.Append(Error);
						}
						FEditorDelegates::PrePIEEnded.Remove(Self->PreEndHandle);
						FEditorDelegates::ShutdownPIE.Remove(Self->ShutdownHandle);
						Self->PreEndHandle.Reset();
						Self->ShutdownHandle.Reset();
					}
				});
				UE::UAI::Contract::FDiagnostic LaunchError;
				if (!LaunchPolicy.RequestPlaySession(Params, [WeakSelf](bool bStarted, const UE::UAI::Contract::FDiagnostic& Error) {
					if (!bStarted)
						if (const auto Self = WeakSelf.Pin()) Self->BeginStopOnGameThread(false, Error);
				}, LaunchError))
				{
					BeginStopOnGameThread(false, MoveTemp(LaunchError));
					return;
				}

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
						StateDiagnostic =
							Native(TEXT("pie_runtime_editor_launch_poll_schedule_failed"), TEXT("The PIE session lifecycle ticker could not be registered."), TEXT("pie_session"), TEXT("lifecycle_ticker_registered"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
						FEditorDelegates::PrePIEEnded.Remove(PreEndHandle);
						FEditorDelegates::ShutdownPIE.Remove(ShutdownHandle);
						PreEndHandle.Reset();
						ShutdownHandle.Reset();
						ReleaseHiddenPieSurfaceOnGameThread();
					}
					else
					{
						FEditorDelegates::PrePIEEnded.Remove(PreEndHandle);
						FEditorDelegates::ShutdownPIE.Remove(ShutdownHandle);
						PreEndHandle.Reset();
						ShutdownHandle.Reset();
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
				if (LocalState == EDebugEditorSessionState::Stopping
					|| LocalState == EDebugEditorSessionState::ExitedFailure
					|| LocalState == EDebugEditorSessionState::ExitedSuccess) return false;
				if (bLocalStopRequested || bCancelled)
				{
					BeginStopOnGameThread(true, UE::UAI::Contract::FDiagnostic());
					return false;
				}

				if (LocalState == EDebugEditorSessionState::Active)
				{
					if (!ActiveWorld.IsValid())
					{
						BeginStopOnGameThread(false, Native(TEXT("pie_runtime_editor_world_lost"), TEXT("The active PIE session lost its world."), TEXT("pie_session"), TEXT("world_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
						return false;
					}
					return true;
				}

				// Hidden windows do not receive the normal visible Slate layout pass.
				// Initialize their native viewport before publishing the paused workspace.
				if (GEditor)
				{
					if (FWorldContext* Context = GEditor->GetPIEWorldContext(PieInstance))
					{
						UGameViewportClient* Client = Context->World()
							? Context->World()->GetGameViewport() : nullptr;
						FSceneViewport* Viewport = Client ? Client->GetGameViewport() : nullptr;
						if (Viewport && Viewport->GetSizeXY() != ViewportSize)
						{
							Viewport->SetViewportSize(ViewportSize.X, ViewportSize.Y);
							if (Viewport->GetSizeXY() != ViewportSize)
							{
								const FIntPoint Actual = Viewport->GetSizeXY();
								BeginStopOnGameThread(false, Native(TEXT("pie_runtime_viewport_size_mismatch"),
									TEXT("The native PIE viewport did not accept the requested pixel dimensions."),
									TEXT("pie_session"), TEXT("viewport_size"),
									Facts({{TEXT("width"), Number(Actual.X)}, {TEXT("height"), Number(Actual.Y)}}),
									Facts({{TEXT("width"), Number(ViewportSize.X)}, {TEXT("height"), Number(ViewportSize.Y)}})));
								return false;
							}
						}
					}
				}
				FUeAgentPieRuntimeDebugSnapshot ActivatedSnapshot;
				UE::UAI::Contract::FDiagnostic ActivationError;
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

			void BeginStopOnGameThread(bool bRequestedSuccess, UE::UAI::Contract::FDiagnostic FailureDiagnostic)
			{
				check(IsInGameThread());
				LaunchPolicy.Disarm();
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
					PendingDiagnostic.Append(FailureDiagnostic);
					ActiveSessionRef = SessionRef;
					LifecycleTicker.Reset();
				}

				if (!ActiveSessionRef.IsEmpty())
				{
					UE::UAI::Contract::FDiagnostic RegistryError;
					if (!Registry->TryEndSession(ActiveSessionRef, RegistryError))
					{
						FScopeLock Lock(&Mutex);
						bTerminalSuccessRequested = false;
						bRegistryCleanupPending = true;
						PendingDiagnostic.Append(RegistryError);
					}
				}

				if (!GUnrealEd)
				{
					CompleteStopOnGameThread(false, Native(TEXT("pie_runtime_editor_stop_unavailable"), TEXT("The Unreal Editor is unavailable to stop the PIE session."), TEXT("pie_session"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
					StateDiagnostic = PendingDiagnostic;
					StateDiagnostic.Append(Native(TEXT("pie_runtime_editor_stop_poll_schedule_failed"), TEXT("The PIE session stop ticker could not be registered."), TEXT("pie_session"), TEXT("stop_ticker_registered"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
					UE::UAI::Contract::FDiagnostic RegistryError;
					if (!Registry->TryEndSession(ActiveSessionRef, RegistryError))
					{
						CompleteStopOnGameThread(false, MoveTemp(RegistryError));
						return false;
					}
					FScopeLock Lock(&Mutex);
					bRegistryCleanupPending = false;
				}
				CompleteStopOnGameThread(true, UE::UAI::Contract::FDiagnostic());
				return false;
			}

			void CompleteStopOnGameThread(bool bCleanupSucceeded, UE::UAI::Contract::FDiagnostic CleanupError)
			{
				FEditorDelegates::PrePIEEnded.Remove(PreEndHandle);
				FEditorDelegates::ShutdownPIE.Remove(ShutdownHandle);
				PreEndHandle.Reset();
				ShutdownHandle.Reset();
				UE::UAI::Contract::FDiagnostic PreferenceError;
				if (!WindowPreferences.Restore(PreferenceError))
				{
					bCleanupSucceeded = false;
					CleanupError.Append(PreferenceError);
				}
				ReleaseHiddenPieSurfaceOnGameThread();
				FScopeLock Lock(&Mutex);
				StopTicker.Reset();
				const bool bSuccess = bCleanupSucceeded
					&& bTerminalSuccessRequested
					&& PendingDiagnostic.IsEmpty();
				State = bSuccess
					? EDebugEditorSessionState::ExitedSuccess
					: EDebugEditorSessionState::ExitedFailure;
				StateDiagnostic = PendingDiagnostic;
                StateDiagnostic.Append(CleanupError);
				if (!bSuccess && StateDiagnostic.IsEmpty())
				{
					StateDiagnostic = Native(TEXT("pie_runtime_editor_stop_failed"), TEXT("The PIE session stopped without satisfying its requested terminal and cleanup state."),
						TEXT("pie_session"), TEXT("stop"),
						Facts({ { TEXT("cleanup_succeeded"), MakeShared<FJsonValueBoolean>(bCleanupSucceeded) }, { TEXT("terminal_success_requested"), MakeShared<FJsonValueBoolean>(bTerminalSuccessRequested) } }),
						Facts({ { TEXT("cleanup_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("terminal_success_requested"), MakeShared<FJsonValueBoolean>(true) } }));
				}
			}

			const int32 PieInstance;
			const int32 LocalPlayerIndex;
			const FIntPoint ViewportSize;
			FPieWindowPreferences WindowPreferences;
			FPieLaunchPolicy LaunchPolicy;
			FDelegateHandle PreEndHandle;
			FDelegateHandle ShutdownHandle;
			TSharedRef<FUeAgentPieRuntimeDebugRegistry, ESPMode::ThreadSafe> Registry;
			FCriticalSection Mutex;
			TUniquePtr<FUeAgentCancellationToken> CancellationToken;
			TWeakObjectPtr<UWorld> ActiveWorld;
			FString NativeResourceId;
			FString SessionRef;
			UE::UAI::Contract::FDiagnostic StateDiagnostic;
			UE::UAI::Contract::FDiagnostic PendingDiagnostic;
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
		FIntPoint ViewportSize;
		if (!TryReadRequest(
				Request, PieInstance, LocalPlayerIndex, ViewportSize, OutError))
		{
			return false;
		}
		OutSession = MakeShared<FPieRuntimeDebugEditorSession, ESPMode::ThreadSafe>(
			PieInstance, LocalPlayerIndex, ViewportSize, MoveTemp(Registry));
		return true;
	}
}
