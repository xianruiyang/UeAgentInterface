#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class SWindow;
class FSceneViewport;

namespace UE::UAI::EditorCapture::SlateWindow
{
	void PumpAndTick();
	bool Prepare(const TSharedPtr<SWindow>& Window, UE::UAI::Contract::FDiagnostic& OutError,
		bool bAllowWindowMutation = true, bool* OutWindowMutated = nullptr);
	bool PrepareViewport(const TSharedPtr<FSceneViewport>& Viewport, UE::UAI::Contract::FDiagnostic& OutError,
		bool bRequireNativeSize = true, bool bSkipIfReady = false);
}
