#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeInputState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"
#include "Core/EditorCapture/UeAgentAsyncCapture.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputKeyEventArgs.h"
#include "Input/HittestGrid.h"
#include "Layout/WidgetPath.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Dom/JsonObject.h"
#include "InputAction.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
    using namespace PieDiagnostics;
    namespace
    {
        FModifierKeysState Modifiers(const FUeAgentPieRuntimeInputState& State)
        {
            return FModifierKeysState(State.Keys.Contains(EKeys::LeftShift), State.Keys.Contains(EKeys::RightShift),
                State.Keys.Contains(EKeys::LeftControl), State.Keys.Contains(EKeys::RightControl),
                State.Keys.Contains(EKeys::LeftAlt), State.Keys.Contains(EKeys::RightAlt),
                State.Keys.Contains(EKeys::LeftCommand), State.Keys.Contains(EKeys::RightCommand), false);
        }

        struct FTarget
        {
            FSceneViewport* Scene = nullptr;
            TSharedPtr<SViewport> Viewport;
            FWidgetPath Focus;
            FInputDeviceId Device;
            int32 User = INDEX_NONE;
            int32 HitTestUser = INDEX_NONE;
        };

        bool ContainsViewport(const FWidgetPath& Path, const TSharedPtr<SViewport>& Viewport)
        {
            for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
                if (Path.Widgets[Index].Widget == Viewport) return true;
            return false;
        }

        bool Resolve(FUeAgentPieRuntimeInputState& State, const FUeAgentPieRuntimeDebugSnapshot& Snapshot,
            FTarget& Target, UE::UAI::Contract::FDiagnostic& Error)
        {
            if (!FSlateApplication::IsInitialized() || !Snapshot.LocalPlayer.IsValid() || !Snapshot.World.IsValid())
            {
                const TCHAR* Field = !FSlateApplication::IsInitialized() ? TEXT("slate_initialized")
                    : !Snapshot.LocalPlayer.IsValid() ? TEXT("local_player_available") : TEXT("world_available");
                Error = Native(TEXT("pie_input_target_unavailable"), TEXT("The PIE input target no longer has its required Slate, local player, or world state."),
                    TEXT("pie_input"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
            auto* Client = Snapshot.World->GetGameViewport();
            Target.Scene = Client ? Client->GetGameViewport() : nullptr;
            Target.Viewport = Target.Scene ? Target.Scene->GetViewportWidget().Pin() : nullptr;
            if (!Target.Viewport.IsValid())
            {
                const TCHAR* Field = !Client ? TEXT("viewport_client_available") : !Target.Scene ? TEXT("scene_viewport_available") : TEXT("viewport_widget_available");
                Error = Native(TEXT("pie_input_viewport_unavailable"), TEXT("The PIE input viewport is unavailable."),
                    TEXT("pie_input"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
            if (Target.Scene->GetSizeXY().X <= 0 || Target.Scene->GetSizeXY().Y <= 0)
            {
                Error = Native(TEXT("pie_input_viewport_size_invalid"), TEXT("The PIE input viewport dimensions must be positive."),
                    TEXT("pie_input"), TEXT("viewport_size"), Size(Target.Scene->GetSizeXY()), MakeShared<FJsonValueString>(TEXT("positive width and height")));
                return false;
            }
            auto& App = FSlateApplication::Get();
            const auto Window = App.FindWidgetWindow(Target.Viewport.ToSharedRef());
            if (Window.IsValid() && !Window->IsVisible())
            {
                // A background PIE window is attached but omitted from normal
                // Slate painting. Rebuild its native geometry and hit-test grid
                // before input, including after an offscreen widget capture.
                if (!State.HiddenWindowSurface.IsValid())
                    State.HiddenWindowSurface = MakeShared<UE::UAI::EditorCapture::FCaptureSurface>();
                if (!State.HiddenWindowSurface->PaintWindow(Window.ToSharedRef(), Error)) return false;
            }
            const auto Physical = Snapshot.LocalPlayer->GetSlateUser();
            // Viewport hit-test entries belong to the local player's painted
            // Slate user. The virtual user only owns our routed input and focus.
            Target.HitTestUser = Physical.IsValid() ? Physical->GetUserIndex() : INDEX_NONE;
            const auto PlayerFocus = Physical.IsValid() ? Physical->GetFocusedWidget() : nullptr;
            if (!State.SlateUser.IsValid())
            {
                // Virtual slots are shared with WidgetInteractionComponent. Select an
                // unowned slot instead of reusing another component's handle.
                for (int32 Slot = 0; !State.SlateUser.IsValid(); ++Slot)
                {
                    auto Candidate = App.FindOrCreateVirtualUser(Slot);
                    if (Candidate.GetSharedReferenceCount() == 1) State.SlateUser = Candidate;
                }
                FWidgetPath Path;
                if (PlayerFocus.IsValid()) App.GeneratePathToWidgetUnchecked(PlayerFocus.ToSharedRef(), Path);
                App.SetUserFocus(State.SlateUser->GetUserIndex(), ContainsViewport(Path, Target.Viewport) ? PlayerFocus : Target.Viewport);
            }
            else if (PlayerFocus.IsValid() && PlayerFocus != State.LastPlayerFocus.Pin())
            {
                // Follow gameplay SetInputMode/SetUserFocus changes, but never
                // import focus from another editor panel or overwrite our own click focus.
                FWidgetPath Path;
                App.GeneratePathToWidgetUnchecked(PlayerFocus.ToSharedRef(), Path);
                if (ContainsViewport(Path, Target.Viewport)) App.SetUserFocus(State.SlateUser->GetUserIndex(), PlayerFocus);
            }
            State.LastPlayerFocus = PlayerFocus;
            Target.User = State.SlateUser->GetUserIndex();
            Target.Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Snapshot.LocalPlayer->GetPlatformUserId());
            if (!Target.Device.IsValid())
            {
                Error = Native(TEXT("pie_input_device_unavailable"), TEXT("The local player has no valid primary input device."),
                    TEXT("pie_input"), TEXT("input_device_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
            const auto User = App.GetUser(Target.User);
            const auto Focus = User.IsValid() ? User->GetFocusedWidget() : nullptr;
            if (Focus.IsValid()) App.GeneratePathToWidgetUnchecked(Focus.ToSharedRef(), Target.Focus);
            if (!ContainsViewport(Target.Focus, Target.Viewport))
                App.GeneratePathToWidgetUnchecked(Target.Viewport.ToSharedRef(), Target.Focus);
            if (!State.bPointerInitialized)
            {
                State.Pointer = FVector2D(Target.Scene->GetSizeXY()) * 0.5;
                State.bPointerInitialized = true;
            }
            if (!Target.Focus.IsValid())
            {
                Error = Native(TEXT("pie_input_focus_path_unavailable"), TEXT("Slate could not resolve a focus path for the PIE viewport."),
                    TEXT("pie_input"), TEXT("focus_path_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
            return true;
        }

        FVector2D Absolute(const FTarget& Target, FVector2D Pixel)
        {
            const FGeometry& Geometry = Target.Viewport->GetCachedGeometry();
            const FVector2D Size(Target.Scene->GetSizeXY());
            return Geometry.LocalToAbsolute(Pixel * Geometry.GetLocalSize() / Size);
        }

        FWidgetPath Hit(const FTarget& Target, FVector2D AbsolutePosition)
        {
            auto& App = FSlateApplication::Get();
            const auto Window = App.FindWidgetWindow(Target.Viewport.ToSharedRef());
            FWidgetPath Path = Window.IsValid() ? FWidgetPath(Window->GetHittestGrid().GetBubblePath(AbsolutePosition, 0.0f, false, Target.HitTestUser)) : FWidgetPath();
            if (ContainsViewport(Path, Target.Viewport)) return Path;

            FWidgetPath ViewportPath;
            App.GeneratePathToWidgetUnchecked(Target.Viewport.ToSharedRef(), ViewportPath);
            // Focus-path construction only fills Widgets. Pointer routing also
            // indexes a corresponding optional virtual position for every widget.
            TArray<FWidgetAndPointer> PointerWidgets;
            PointerWidgets.Reserve(ViewportPath.Widgets.Num());
            for (int32 Index = 0; Index < ViewportPath.Widgets.Num(); ++Index)
                PointerWidgets.Emplace(ViewportPath.Widgets[Index]);
            return FWidgetPath(PointerWidgets);
        }

        TSet<FKey> Buttons(const FUeAgentPieRuntimeInputState& State)
        {
            TSet<FKey> Result;
            for (const FKey& Key : State.Keys) if (Key.IsMouseButton()) Result.Add(Key);
            return Result;
        }

        void RouteCharacter(const FTarget& Target, const FUeAgentPieRuntimeInputState& State,
            TCHAR Character, bool bRepeat = false)
        {
            const FCharacterEvent Event(Character, Modifiers(State), Target.User, bRepeat);
            FReply Reply = FReply::Unhandled();
            for (int32 I = Target.Focus.Widgets.Num() - 1; I >= 0
                && Target.Focus.Widgets[I].Widget != Target.Viewport && !Reply.IsEventHandled(); --I)
                Reply = Target.Focus.Widgets[I].Widget->OnKeyChar(Target.Focus.Widgets[I].Geometry, Event);
            FSlateApplication::Get().ProcessReply(Target.Focus, Reply, nullptr, nullptr, Target.User);
        }
    }

    bool FUeAgentPieRuntimeInputState::Key(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, FKey KeyValue,
        EInputEvent Event, double Amount, UE::UAI::Contract::FDiagnostic& Error)
    {
        if (!Snapshot.PlayerController.IsValid())
        {
            Error = Native(TEXT("pie_input_player_unavailable"), TEXT("The PIE player controller is no longer available."),
                TEXT("pie_input"), TEXT("player_controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            return false;
        }
        if (Event == IE_Axis)
        {
            if (Snapshot.World.IsValid() && Snapshot.World->GetGameViewport() && !Snapshot.World->GetGameViewport()->IgnoreInput())
                Snapshot.PlayerController->InputKey(FInputKeyEventArgs::CreateSimulated(KeyValue, Event, Amount));
            return true;
        }
        FTarget Target;
        if (!Resolve(*this, Snapshot, Target, Error)) return false;
        const bool bControl = Keys.Contains(EKeys::LeftControl) || Keys.Contains(EKeys::RightControl)
            || Keys.Contains(EKeys::LeftCommand) || Keys.Contains(EKeys::RightCommand);
        const bool bShift = Keys.Contains(EKeys::LeftShift) || Keys.Contains(EKeys::RightShift);
        if (Event != IE_Released && Target.Focus.GetLastWidget() != Target.Viewport
            && ((bControl && (KeyValue == EKeys::C || KeyValue == EKeys::V || KeyValue == EKeys::X || KeyValue == EKeys::Insert))
                || (bShift && (KeyValue == EKeys::Insert || KeyValue == EKeys::Delete))))
        {
            Error = Native(TEXT("pie_input_system_clipboard_not_supported"), TEXT("PIE virtual input does not support system clipboard shortcuts in focused widgets."),
                TEXT("pie_input"), TEXT("clipboard_shortcut"),
                Facts({ { TEXT("key"), MakeShared<FJsonValueString>(KeyValue.ToString()) }, { TEXT("control_or_command"), MakeShared<FJsonValueBoolean>(bControl) }, { TEXT("shift"), MakeShared<FJsonValueBoolean>(bShift) } }),
                MakeShared<FJsonValueString>(TEXT("input without a system clipboard shortcut")));
            return false;
        }
        if (Event == IE_Released) Keys.Remove(KeyValue); else Keys.Add(KeyValue);
        auto& App = FSlateApplication::Get();
        if (KeyValue.IsMouseButton())
        {
            const auto Position = Absolute(Target, Pointer);
            const FPointerEvent EventValue(Target.Device, 0, Position, Position, Buttons(*this), KeyValue, 0, Modifiers(*this), Target.User);
            const auto Path = Hit(Target, Position);
            if (Event == IE_Released) App.RoutePointerUpEvent(Path, EventValue);
            else if (Event == IE_DoubleClick) App.RoutePointerDoubleClickEvent(Path, EventValue);
            else App.RoutePointerDownEvent(Path, EventValue);
            return true;
        }
        const uint32* KeyCode = nullptr;
        const uint32* Character = nullptr;
        FInputKeyManager::Get().GetCodesFromKey(KeyValue, KeyCode, Character);
        FKeyEvent KeyEvent(KeyValue, Modifiers(*this), Target.Device, Event == IE_Repeat,
            Character ? *Character : 0, KeyCode ? *KeyCode : 0, Target.User);
        // Keep propagation within the PIE subtree; editor-global shortcuts and
        // physical-user input preprocessors are not this virtual input target.
        FReply Reply = FReply::Unhandled();
        int32 Root = 0;
        while (Root < Target.Focus.Widgets.Num() && Target.Focus.Widgets[Root].Widget != Target.Viewport) ++Root;
        if (Event != IE_Released)
            for (int32 I = Root; I < Target.Focus.Widgets.Num() && !Reply.IsEventHandled(); ++I)
                Reply = Target.Focus.Widgets[I].Widget->OnPreviewKeyDown(Target.Focus.Widgets[I].Geometry, KeyEvent);
        for (int32 I = Target.Focus.Widgets.Num() - 1; I >= Root && !Reply.IsEventHandled(); --I)
            Reply = Event == IE_Released ? Target.Focus.Widgets[I].Widget->OnKeyUp(Target.Focus.Widgets[I].Geometry, KeyEvent)
                : Target.Focus.Widgets[I].Widget->OnKeyDown(Target.Focus.Widgets[I].Geometry, KeyEvent);
        App.ProcessReply(Target.Focus, Reply, nullptr, nullptr, Target.User);
        if (Event != IE_Released)
        {
            // Slate's editable text handles ordinary Backspace through KeyChar,
            // whereas Ctrl+Backspace and Enter are handled by KeyDown. Mirror
            // their control characters without guessing printable keyboard layout.
            const bool bAlt = Keys.Contains(EKeys::LeftAlt) || Keys.Contains(EKeys::RightAlt);
            TCHAR Control = 0;
            if (KeyValue == EKeys::BackSpace && !bAlt) Control = bControl ? TCHAR(127) : TCHAR(8);
            else if (KeyValue == EKeys::Enter) Control = TCHAR('\n');
            else if (KeyValue == EKeys::Tab && !bControl && !bAlt) Control = TCHAR('\t');
            if (Control) RouteCharacter(Target, *this, Control, Event == IE_Repeat);
        }
        return true;
    }

    bool FUeAgentPieRuntimeInputState::MovePointer(const FUeAgentPieRuntimeDebugSnapshot& Snapshot,
        FVector2D Value, bool bRelative, UE::UAI::Contract::FDiagnostic& Error)
    {
        FTarget Target;
        if (!Resolve(*this, Snapshot, Target, Error)) return false;
        if (bRelative && !Snapshot.PlayerController->bShowMouseCursor && !Snapshot.World->GetGameViewport()->IgnoreInput())
        {
            Snapshot.PlayerController->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseX, IE_Axis, Value.X));
            Snapshot.PlayerController->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseY, IE_Axis, -Value.Y));
            return true;
        }
        const FVector2D Previous = Absolute(Target, Pointer);
        Pointer = bRelative ? Pointer + Value : Value;
        const FVector2D Position = Absolute(Target, Pointer);
        const FPointerEvent Event(Target.Device, 0, Position, Previous, Buttons(*this), EKeys::Invalid, 0, Modifiers(*this), Target.User);
        FSlateApplication::Get().RoutePointerMoveEvent(Hit(Target, Position), Event, true);
        return true;
    }

    bool FUeAgentPieRuntimeInputState::Scroll(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, double Delta, UE::UAI::Contract::FDiagnostic& Error)
    {
        FTarget Target;
        if (!Resolve(*this, Snapshot, Target, Error)) return false;
        const FVector2D Position = Absolute(Target, Pointer);
        const FPointerEvent Event(Target.Device, 0, Position, Position, Buttons(*this), EKeys::MouseWheelAxis, Delta, Modifiers(*this), Target.User);
        FSlateApplication::Get().RouteMouseWheelOrGestureEvent(Hit(Target, Position), Event, nullptr);
        return true;
    }

    bool FUeAgentPieRuntimeInputState::Text(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, const FString& Value, UE::UAI::Contract::FDiagnostic& Error)
    {
        FTarget Target;
        if (!Resolve(*this, Snapshot, Target, Error)) return false;
        if (Target.Focus.GetLastWidget() == Target.Viewport)
        {
            Error = Native(TEXT("pie_input_text_focus_unavailable"), TEXT("Text input requires a focused widget inside the PIE viewport."),
                TEXT("pie_input"), TEXT("focus_is_viewport"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
            return false;
        }
        for (TCHAR Character : Value)
            RouteCharacter(Target, *this, Character);
        return true;
    }

    void FUeAgentPieRuntimeInputState::ReleaseSlate()
    {
        HiddenWindowSurface.Reset();
        if (SlateUser.IsValid() && FSlateApplication::IsInitialized())
        {
            const auto User = FSlateApplication::Get().GetUser(SlateUser->GetUserIndex());
            if (User.IsValid()) { User->ReleaseAllCapture(); User->ClearFocus(); }
        }
        SlateUser.Reset();
        LastPlayerFocus.Reset();
    }

    TSharedRef<FJsonObject> FUeAgentPieRuntimeInputState::Describe(const FUeAgentPieRuntimeDebugSnapshot& Snapshot)
    {
        const auto Result = MakeShared<FJsonObject>();
        TArray<FKey> Registered;
        EKeys::GetAllKeys(Registered);
        Registered.Sort([](const FKey& A, const FKey& B) { return A.ToString() < B.ToString(); });
        const auto Catalog = MakeShared<FJsonObject>();
        for (const FKey& Key : Registered)
        {
            const auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("display_name"), Key.GetDisplayName().ToString());
            Entry->SetBoolField(TEXT("digital"), Key.IsDigital());
            Entry->SetNumberField(TEXT("axis_dimensions"), Key.IsAxis3D() ? 3 : Key.IsAxis2D() ? 2 : Key.IsAxis1D() ? 1 : 0);
            Entry->SetBoolField(TEXT("mouse_button"), Key.IsMouseButton());
            Entry->SetBoolField(TEXT("gamepad"), Key.IsGamepadKey());
            Entry->SetStringField(TEXT("route"), Key.IsAxis2D() || Key.IsAxis3D() ? TEXT("device_vector_not_implemented") : TEXT("input_key"));
            Catalog->SetObjectField(Key.ToString(), Entry);
        }
        FString Content;
        FJsonSerializer::Serialize(Catalog, TJsonWriterFactory<>::Create(&Content));
        const FString CatalogPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UeAgentInterface/RuntimeDebug/input_keys.hjson"));
        bool bCatalogReady = Content == KeyCatalogContent && IFileManager::Get().FileExists(*CatalogPath);
        if (!bCatalogReady)
        {
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(CatalogPath), true);
            bCatalogReady = FFileHelper::SaveStringToFile(Content, *CatalogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            if (bCatalogReady) KeyCatalogContent = MoveTemp(Content);
        }
        if (bCatalogReady) Result->SetStringField(TEXT("key_catalog_path"), CatalogPath);
        else Result->SetStringField(TEXT("key_catalog_error"), TEXT("input_key_catalog_write_failed"));
        TArray<TSharedPtr<FJsonValue>> Pressed;
        TArray<FString> Names;
        for (const auto& Key : Keys) Names.Add(Key.ToString());
        Names.Sort();
        for (const auto& Name : Names) Pressed.Add(MakeShared<FJsonValueString>(Name));
        Result->SetArrayField(TEXT("held_keys"), Pressed);
        const auto AxisValues = MakeShared<FJsonObject>();
        for (const auto& Axis : Axes) AxisValues->SetNumberField(Axis.Key.ToString(), Axis.Value);
        Result->SetObjectField(TEXT("held_axes"), AxisValues);
        Result->SetArrayField(TEXT("pointer_pixels"), {MakeShared<FJsonValueNumber>(Pointer.X), MakeShared<FJsonValueNumber>(Pointer.Y)});
        Result->SetBoolField(TEXT("pointer_initialized"), bPointerInitialized);
        Result->SetStringField(TEXT("pointer_frame"), TEXT("pie_viewport_pixels_top_left_right_down"));
        if (Snapshot.World.IsValid() && Snapshot.World->GetGameViewport())
        {
            auto* Client = Snapshot.World->GetGameViewport();
            Result->SetBoolField(TEXT("ignore_game_input"), Client->IgnoreInput());
            if (Client->GetGameViewport())
            {
                const auto Size = Client->GetGameViewport()->GetSizeXY();
                Result->SetArrayField(TEXT("viewport_size"), {MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
            }
        }
        if (Snapshot.PlayerController.IsValid()) Result->SetBoolField(TEXT("show_mouse_cursor"), Snapshot.PlayerController->bShowMouseCursor);
        if (FSlateApplication::IsInitialized())
        {
            const auto User = SlateUser.IsValid() ? FSlateApplication::Get().GetUser(SlateUser->GetUserIndex())
                : Snapshot.LocalPlayer.IsValid() ? Snapshot.LocalPlayer->GetSlateUser() : nullptr;
            if (User.IsValid())
            {
                const auto Focus = User->GetFocusedWidget();
                if (Focus.IsValid()) Result->SetStringField(TEXT("focus_widget_type"), Focus->GetTypeAsString());
                Result->SetBoolField(TEXT("has_pointer_capture"), User->HasAnyCapture());
            }
        }
        return Result;
    }
}
