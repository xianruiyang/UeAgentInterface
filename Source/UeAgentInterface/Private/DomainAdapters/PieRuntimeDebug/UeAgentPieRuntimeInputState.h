#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "InputActionValue.h"

class UInputAction;
class FSlateVirtualUserHandle;
class FJsonObject;
class SWidget;

namespace UE::UAI::EditorCapture { class FCaptureSurface; }

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
    struct FUeAgentPieRuntimeDebugSnapshot;
    struct FUeAgentPieRuntimeInputState
    {
        TSet<FKey> Keys;
        TMap<FKey, double> Axes;
        TMap<TWeakObjectPtr<const UInputAction>, FInputActionValue> Actions;
        TSharedPtr<FSlateVirtualUserHandle> SlateUser;
        TSharedPtr<UE::UAI::EditorCapture::FCaptureSurface> HiddenWindowSurface;
        TWeakPtr<SWidget> LastPlayerFocus;
        FVector2D Pointer = FVector2D::ZeroVector;
        bool bPointerInitialized = false;
        FString KeyCatalogContent;

        bool Key(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, FKey Key, EInputEvent Event, double Amount, UE::UAI::Contract::FDiagnostic& Error);
        bool MovePointer(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, FVector2D Value, bool bRelative, UE::UAI::Contract::FDiagnostic& Error);
        bool Scroll(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, double Delta, UE::UAI::Contract::FDiagnostic& Error);
        bool Text(const FUeAgentPieRuntimeDebugSnapshot& Snapshot, const FString& Value, UE::UAI::Contract::FDiagnostic& Error);
        void ReleaseSlate();
        TSharedRef<FJsonObject> Describe(const FUeAgentPieRuntimeDebugSnapshot& Snapshot);
    };
}
