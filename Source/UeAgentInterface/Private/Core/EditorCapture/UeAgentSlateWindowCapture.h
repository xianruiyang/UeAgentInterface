#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class SWindow;
class FSceneViewport;
class FJsonObject;

namespace UE::UAI::EditorCapture::SlateWindow
{
	void PumpAndTick();
	bool Prepare(const TSharedPtr<SWindow>& Window, UE::UAI::Contract::FDiagnostic& OutError,
		bool bAllowWindowMutation = true, bool* OutWindowMutated = nullptr);
	bool PrepareViewport(const TSharedPtr<FSceneViewport>& Viewport, UE::UAI::Contract::FDiagnostic& OutError,
		bool bRequireNativeSize = true);

	/** Initializes only a zero-extent viewport; never changes its native window. */
	class FScopedViewportExtent final
	{
	public:
		FScopedViewportExtent() = default;
		~FScopedViewportExtent();
		FScopedViewportExtent(const FScopedViewportExtent&) = delete;
		FScopedViewportExtent& operator=(const FScopedViewportExtent&) = delete;
		bool Prepare(const TSharedPtr<FSceneViewport>& InViewport, const FIntPoint* ColdStartSize,
			const FString& ColdStartSource, UE::UAI::Contract::FDiagnostic& OutError);
		void Restore();
		TSharedPtr<FJsonObject> Describe() const;
	private:
		TSharedPtr<FSceneViewport> Viewport;
		FIntPoint ExtentBefore = FIntPoint::ZeroValue;
		FIntPoint RequestedExtent = FIntPoint::ZeroValue;
		FString ExtentSource = TEXT("existing_viewport");
		bool bFixedBefore = false;
		bool bInitialized = false;
		bool bTemporaryFixed = false;
	};
}
